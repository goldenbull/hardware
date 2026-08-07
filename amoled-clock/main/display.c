#include "display.h"

#include "board_pins.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "display";

#define LVGL_TICK_PERIOD_MS    2
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS 1
#define LVGL_TASK_STACK_SIZE   (6 * 1024)
#define LVGL_TASK_PRIORITY     2

/* 屏内亮度寄存器（Write Display Brightness） */
#define SH8601_CMD_WRITE_BRIGHTNESS 0x51

/*
 * QSPI 模式下命令要包成 0x02<<24 | cmd<<8 才会被面板认出来。
 * SH8601 驱动内部的 tx_param() 做了这层封装，但没导出，所以这里自己包一遍——
 * 直接把裸 0x51 丢给 esp_lcd_panel_io_tx_param 发出去的是 0x00000051，
 * 操作码字节是 0x00，面板整条命令都会丢掉。
 */
#define SH8601_QSPI_CMD(cmd) ((0x02 << 24) | ((cmd) << 8))

/* 0x28/0x29/0x51 之后面板需要的建立时间，取自厂商初始化序列里的延时 */
#define PANEL_SETTLE_MS 10

/*
 * 每块绘制缓冲的高度（行）。缓冲必须放在内部 DMA RAM 里：
 * IDF 5.5.4 的 esp_lcd SPI panel IO 不会给事务打上 SPI_TRANS_DMA_USE_PSRAM，
 * 所以 tx_buffer 只要落在 PSRAM，spi_master 就会另外申请一块等长的内部 DMA
 * 临时缓冲（spi_master.c 的 setup_dma_priv_buffer）。整屏 251KB 必然申请失败。
 * 536*24*2 = 25.7KB/块，两块约 50KB，宽度是偶数所以长度天然 4 字节对齐。
 */
#define LVGL_BUF_LINES 24
#define LVGL_BUF_PIXELS (LCD_H_RES * LVGL_BUF_LINES)

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t    s_panel;
static SemaphoreHandle_t         s_lvgl_mux;
static uint8_t                   s_brightness = DISPLAY_BRIGHTNESS_DEFAULT;
static bool                      s_on         = true;

/*
 * SH8601 厂商初始化序列，取自官方出厂固件。
 * 0x2A/0x2B 把可视窗口写死成 0..535 x 0..239，0x51 是亮度。
 */
static const sh8601_lcd_init_cmd_t s_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x36, (uint8_t[]){0xF0}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},                    /* 16bit RGB565 */
    {0x2A, (uint8_t[]){0x00, 0x00, 0x02, 0x17}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x00, 0xEF}, 4, 0},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){DISPLAY_BRIGHTNESS_DEFAULT}, 1, 0},
};

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx)
{
    lv_disp_drv_t *disp_drv = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_drv);
    return false;
}

/*
 * 分块刷新，area 是本次重绘的局部区域。
 * draw_bitmap 是异步的：DMA 送完后 on_color_trans_done 通知 LVGL，
 * LVGL 同时已经在另一块缓冲上画下一块了。
 */
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
    if (err != ESP_OK) {
        /*
         * 送屏失败就不会有 on_color_trans_done，LVGL 会永远等在这一帧上。
         * 自己补一次 flush_ready，让 UI 继续跑（屏是花的，但至少能看日志）。
         */
        ESP_LOGE(TAG, "draw_bitmap 失败：%s", esp_err_to_name(err));
        lv_disp_flush_ready(drv);
    }
}

/* SH8601 要求刷新窗口的边界对齐到偶数，不做这个会花屏 */
static void lvgl_rounder_cb(lv_disp_drv_t *drv, lv_area_t *area)
{
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

bool display_lock(int timeout_ms)
{
    const TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lvgl_mux, ticks) == pdTRUE;
}

void display_unlock(void)
{
    xSemaphoreGiveRecursive(s_lvgl_mux);
}

static void lvgl_task(void *arg)
{
    uint32_t delay_ms = LVGL_TASK_MAX_DELAY_MS;

    while (1) {
        if (display_lock(-1)) {
            delay_ms = lv_timer_handler();
            display_unlock();
        }
        if (delay_ms > LVGL_TASK_MAX_DELAY_MS) {
            delay_ms = LVGL_TASK_MAX_DELAY_MS;
        } else if (delay_ms < LVGL_TASK_MIN_DELAY_MS) {
            delay_ms = LVGL_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "初始化 QSPI 总线");
    const spi_bus_config_t bus_cfg = SH8601_PANEL_BUS_QSPI_CONFIG(
        PIN_NUM_LCD_PCLK,
        PIN_NUM_LCD_DATA0,
        PIN_NUM_LCD_DATA1,
        PIN_NUM_LCD_DATA2,
        PIN_NUM_LCD_DATA3,
        LVGL_BUF_PIXELS * LCD_BIT_PER_PIXEL / 8);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO), TAG, "spi_bus_initialize failed");

    ESP_LOGI(TAG, "安装 panel IO");
    static lv_disp_drv_t disp_drv;   /* flush_ready 回调要用，必须是静态存储 */
    const esp_lcd_panel_io_spi_config_t io_cfg =
        SH8601_PANEL_IO_QSPI_CONFIG(PIN_NUM_LCD_CS, on_color_trans_done, &disp_drv);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io),
                        TAG, "esp_lcd_new_panel_io_spi failed");

    sh8601_vendor_config_t vendor_cfg = {
        .init_cmds = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BIT_PER_PIXEL,
        .vendor_config = &vendor_cfg,
    };

    ESP_LOGI(TAG, "安装 SH8601 驱动");
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_sh8601(s_io, &panel_cfg, &s_panel), TAG, "esp_lcd_new_panel_sh8601 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "panel on failed");

    lv_init();

    /* 两块分块绘制缓冲，必须是内部 DMA RAM，理由见 LVGL_BUF_LINES 处的注释 */
    const size_t buf_bytes = LVGL_BUF_PIXELS * sizeof(lv_color_t);
    lv_color_t *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    lv_color_t *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(buf1 && buf2, ESP_ERR_NO_MEM, TAG, "分配绘制缓冲失败，内部 DMA RAM 不足");

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LVGL_BUF_PIXELS);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.rounder_cb = lvgl_rounder_cb;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.user_data = s_panel;
    /*
     * 分块刷新：整屏缓冲要 251KB，只能放 PSRAM，而 PSRAM 缓冲过不了 SPI DMA
     * （见 LVGL_BUF_LINES 的注释），所以这里不能开 full_refresh。
     */
    disp_drv.full_refresh = 0;
    lv_disp_drv_register(&disp_drv);

    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick_timer), TAG, "esp_timer_create failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000), TAG, "esp_timer_start failed");

    s_lvgl_mux = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_lvgl_mux, ESP_ERR_NO_MEM, TAG, "创建 LVGL 互斥锁失败");

    ESP_RETURN_ON_FALSE(xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "创建 LVGL 任务失败");

    ESP_LOGI(TAG, "显示就绪：%dx%d，内部 DMA 双缓冲 %d 行 / %d KB x2",
             LCD_H_RES, LCD_V_RES, LVGL_BUF_LINES, (int)(buf_bytes / 1024));
    return ESP_OK;
}

/*
 * 面板 IO 不是线程安全的：esp_lcd 的 panel_io_spi_tx_param() 会读写
 * num_trans_inflight 并复用 trans_pool[0]，而这些正是 LVGL 任务刷屏时
 * tx_color() 在用的东西。触摸任务直接调过来就会踩到刷屏中的事务，
 * 命令被冲掉（表现为点了没反应）。这里借 LVGL 的锁把两边串起来——
 * 刷屏发生在 lv_timer_handler() 里，而它全程持有这把锁。
 */
static void panel_write_brightness(uint8_t brightness)
{
    display_lock(-1);
    esp_lcd_panel_io_tx_param(s_io, SH8601_QSPI_CMD(SH8601_CMD_WRITE_BRIGHTNESS), &brightness, 1);
    display_unlock();
}

static void panel_disp_on_off(bool on)
{
    display_lock(-1);
    esp_lcd_panel_disp_on_off(s_panel, on);
    display_unlock();
}

void display_set_brightness(uint8_t brightness)
{
    if (brightness < DISPLAY_BRIGHTNESS_MIN) {
        brightness = DISPLAY_BRIGHTNESS_MIN;
    }

    s_brightness = brightness;
    if (s_on) {
        panel_write_brightness(brightness);
    }
}

uint8_t display_get_brightness(void)
{
    return s_brightness;
}

/*
 * 注意这里没有 "if (on == s_on) return;" 的早退：调用方传什么状态就照发一遍。
 * 早退省下的那点命令不值得——万一面板漏了一次 0x29，早退会让软件状态和面板
 * 实际状态永久对不上，而重发 0x28/0x29 对面板本来就是幂等的。
 *
 * 顺序也调成了先写亮度、再开关显示，两个分支对称：
 *   开：0x51(目标亮度) → 等 → 0x29
 *   关：0x51(0)        → 等 → 0x28
 * 这样"显示已开但亮度还是 0"这个状态根本不存在——亮度是在显示关着的时候就
 * 设好的，0x29 一开就是对的值，也不会有亮度突变。厂商初始化序列同样是先写
 * 0x51 再发 0x29，中间留 10ms（见 s_init_cmds）。延时放在锁外，不卡 LVGL。
 */
void display_set_on(bool on)
{
    s_on = on;

    panel_write_brightness(on ? s_brightness : 0);
    vTaskDelay(pdMS_TO_TICKS(PANEL_SETTLE_MS));
    panel_disp_on_off(on);

    ESP_LOGI(TAG, "屏幕 %s", on ? "开" : "关");
}

bool display_is_on(void)
{
    return s_on;
}
