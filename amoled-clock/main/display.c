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
 * full_refresh 模式下 area 恒为整屏，这里就是"把内存里画好的整帧发出去"。
 * draw_bitmap 是异步的：DMA 送完后 on_color_trans_done 通知 LVGL，
 * LVGL 同时已经在另一块缓冲上画下一帧了。
 */
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
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
        LCD_H_RES * LCD_V_RES * LCD_BIT_PER_PIXEL / 8);
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

    /*
     * 两块整屏缓冲，每块 536*240*2 = 251KB，放 PSRAM。
     * ESP32-S3 的 GDMA 可以直接从 PSRAM 取数，不需要再往内部 RAM 搬一次。
     */
    const size_t buf_pixels = LCD_H_RES * LCD_V_RES;
    lv_color_t *buf1 = heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_color_t *buf2 = heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(buf1 && buf2, ESP_ERR_NO_MEM, TAG, "分配整屏缓冲失败，检查 PSRAM 是否使能");

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_pixels);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.rounder_cb = lvgl_rounder_cb;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.user_data = s_panel;
    disp_drv.full_refresh = 1;   /* 整屏合成后一次性送屏，杜绝局部擦除造成的闪烁 */
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

    ESP_LOGI(TAG, "显示就绪：%dx%d，整屏双缓冲 %d KB x2",
             LCD_H_RES, LCD_V_RES, (int)(buf_pixels * sizeof(lv_color_t) / 1024));
    return ESP_OK;
}

void display_set_brightness(uint8_t brightness)
{
    if (brightness < DISPLAY_BRIGHTNESS_MIN) {
        brightness = DISPLAY_BRIGHTNESS_MIN;
    }
    s_brightness = brightness;
    if (s_on) {
        const uint8_t param = brightness;
        esp_lcd_panel_io_tx_param(s_io, SH8601_CMD_WRITE_BRIGHTNESS, &param, 1);
    }
}

uint8_t display_get_brightness(void)
{
    return s_brightness;
}

void display_set_on(bool on)
{
    if (on == s_on) {
        return;
    }
    s_on = on;

    if (on) {
        esp_lcd_panel_disp_on_off(s_panel, true);
        const uint8_t param = s_brightness;
        esp_lcd_panel_io_tx_param(s_io, SH8601_CMD_WRITE_BRIGHTNESS, &param, 1);
    } else {
        /* 先把亮度压到 0 再关显示，避免关屏瞬间的余辉 */
        const uint8_t param = 0;
        esp_lcd_panel_io_tx_param(s_io, SH8601_CMD_WRITE_BRIGHTNESS, &param, 1);
        esp_lcd_panel_disp_on_off(s_panel, false);
    }
    ESP_LOGI(TAG, "屏幕 %s", on ? "开" : "关");
}

bool display_is_on(void)
{
    return s_on;
}
