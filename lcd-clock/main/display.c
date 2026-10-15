#include "display.h"

#include "board_pins.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
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
/* 比麦克风任务低、比写卡任务低：界面卡一下无所谓，录音不能断 */
#define LVGL_TASK_PRIORITY     2

/*
 * 绘制缓冲放内部 DMA RAM。放 PSRAM 的话 spi_master 每次都要另申请一块等长的
 * 内部临时缓冲再拷一遍（amoled-clock 的 display.c 里有同样的说明）。
 * 320*40*2 = 25KB/块，两块 50KB，C5 的 384KB SRAM 放得下。
 */
#define LVGL_BUF_LINES  40
#define LVGL_BUF_PIXELS (LCD_H_RES * LVGL_BUF_LINES)

static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t         s_lvgl_mux;

/*
 * ST7796 不用 espressif/esp_lcd_st7796 组件：它 1.0.0 还在用 IDF 6 删掉的
 * esp_lcd_panel_dev_config_t.color_space，编不过；1.1 以后的版本又没把 esp32c5
 * 列进 targets，组件管理器不让装。屏的操作本来就只有"发初始化序列"和
 * "CASET/RASET/RAMWR 刷一块"，直接用 panel IO 写反而更清楚。
 *
 * 初始化序列 = 组件 1.0.0 的 panel_st7796_reset/init（软复位、退出睡眠、
 * MADCTL、COLMOD、厂商序列）+ 官方 BSP 在 init 之后追加的反色、开显示、X 镜像。
 * BSP 里 BGR 和 X 镜像是分两次写 MADCTL 的，这里合成一次 0x48。
 */
#define ST7796_MADCTL_MX   0x40
#define ST7796_MADCTL_BGR  0x08

typedef struct {
    uint8_t  cmd;
    uint8_t  data[14];
    uint8_t  len;
    uint16_t delay_ms;
} lcd_init_cmd_t;

static const lcd_init_cmd_t s_init_cmds[] = {
    /* 复位后要等 120ms 才能发 SLPOUT，SLPOUT 之后同样要等 120ms */
    {LCD_CMD_SWRESET, {0}, 0, 120},
    {LCD_CMD_SLPOUT, {0}, 0, 120},
    {LCD_CMD_MADCTL, {ST7796_MADCTL_MX | ST7796_MADCTL_BGR}, 1, 0},
    {LCD_CMD_COLMOD, {0x55}, 1, 0},     /* RGB565 */
    /* 厂商序列：0xF0 C3/96 解锁扩展命令，最后 3C/69 再锁上 */
    {0xF0, {0xC3}, 1, 0},
    {0xF0, {0x96}, 1, 0},
    {0xB4, {0x01}, 1, 0},
    {0xB7, {0xC6}, 1, 0},
    {0xE8, {0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33}, 8, 0},
    {0xC1, {0x06}, 1, 0},
    {0xC2, {0xA7}, 1, 0},
    {0xC5, {0x18}, 1, 0},
    {0xE0, {0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B}, 14, 0},
    {0xE1, {0xF0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2D, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B}, 14, 0},
    {0xF0, {0x3C}, 1, 0},
    {0xF0, {0x69}, 1, 0},
    {LCD_CMD_INVON, {0}, 0, 0},         /* 这块 IPS 面板要开反色，颜色才是对的 */
    {LCD_CMD_DISPON, {0}, 0, 20},
};

static esp_err_t panel_init(void)
{
    for (size_t i = 0; i < sizeof(s_init_cmds) / sizeof(s_init_cmds[0]); i++) {
        const lcd_init_cmd_t *c = &s_init_cmds[i];
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, c->cmd, c->len ? c->data : NULL, c->len),
                            TAG, "发送命令 0x%02X 失败", c->cmd);
        if (c->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }
    return ESP_OK;
}

/* 设好窗口再把像素丢给 DMA，异步返回，送完了 on_color_trans_done 通知 LVGL */
static esp_err_t panel_draw(int x1, int y1, int x2, int y2, const void *pixels)
{
    const uint8_t caset[] = {x1 >> 8, x1 & 0xFF, x2 >> 8, x2 & 0xFF};
    const uint8_t raset[] = {y1 >> 8, y1 & 0xFF, y2 >> 8, y2 & 0xFF};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, LCD_CMD_CASET, caset, sizeof(caset)), TAG, "CASET failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, LCD_CMD_RASET, raset, sizeof(raset)), TAG, "RASET failed");
    const size_t len = (size_t)(x2 - x1 + 1) * (y2 - y1 + 1) * LCD_BIT_PER_PIXEL / 8;
    return esp_lcd_panel_io_tx_color(s_io, LCD_CMD_RAMWR, pixels, len);
}

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx)
{
    lv_disp_drv_t *disp_drv = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_drv);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_err_t err = panel_draw(area->x1, area->y1, area->x2, area->y2, color_map);
    if (err != ESP_OK) {
        /* 送屏失败就等不到 on_color_trans_done，自己补一次，免得 LVGL 卡死在这一帧 */
        ESP_LOGE(TAG, "draw_bitmap 失败：%s", esp_err_to_name(err));
        lv_disp_flush_ready(drv);
    }
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
    const size_t buf_bytes = LVGL_BUF_PIXELS * sizeof(lv_color_t);

    ESP_LOGI(TAG, "初始化 SPI2 总线（屏和 TF 卡共用）");
    const spi_bus_config_t bus_cfg = {
        .sclk_io_num = PIN_NUM_SPI_SCLK,
        .mosi_io_num = PIN_NUM_SPI_MOSI,
        .miso_io_num = PIN_NUM_SPI_MISO,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = buf_bytes,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI_HOST_SHARED, &bus_cfg, SPI_DMA_CH_AUTO), TAG, "spi_bus_initialize failed");

    static lv_disp_drv_t disp_drv;   /* flush_ready 回调要用，必须是静态存储 */
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_NUM_LCD_DC,
        .cs_gpio_num = PIN_NUM_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 2,
        .on_color_trans_done = on_color_trans_done,
        .user_ctx = &disp_drv,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI_HOST_SHARED, &io_cfg, &s_io),
                        TAG, "esp_lcd_new_panel_io_spi failed");

    /* 硬复位已经在 exio_init() 里做过，序列里再补一次软复位 */
    ESP_RETURN_ON_ERROR(panel_init(), TAG, "ST7796 初始化失败");

    lv_init();

    lv_color_t *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    lv_color_t *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(buf1 && buf2, ESP_ERR_NO_MEM, TAG, "分配绘制缓冲失败，内部 DMA RAM 不足");

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LVGL_BUF_PIXELS);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
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
