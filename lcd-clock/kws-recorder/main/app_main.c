/*
 * Waveshare ESP32-C5-Touch-LCD-3.5 关键词训练数据采集
 *
 * 用板载麦克风把"打开屏幕"/"关闭屏幕"录成长 WAV 存到 TF 卡，拿到 Mac 上切片训练。
 * 采集链路（mic.c）以后原样用在识别固件里，保证训练和识别听到的是同一种声音。
 *
 * 任务划分（单核，优先级从高到低）：
 *   rec_cap —— 6，每 20ms 读 I2S，算电平，录音中送进环形缓冲
 *   rec_wr  —— 4，环形缓冲 → TF 卡
 *   lvgl    —— 2，界面和触摸
 */
#include "display.h"
#include "esp_err.h"
#include "esp_log.h"
#include "exio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "mic.h"
#include "recorder.h"
#include "sdcard.h"
#include "touch.h"
#include "ui.h"

static const char *TAG = "app";

#define BACKLIGHT_PERCENT 80

void app_main(void)
{
    ESP_ERROR_CHECK(i2c_bus_init());
    ESP_ERROR_CHECK(exio_init());       /* 放开屏和触摸的复位，必须在两者之前 */
    ESP_ERROR_CHECK(display_init());    /* 顺带初始化 TF 卡要用的 SPI 总线 */
    if (touch_init(true) != ESP_OK) {
        ESP_LOGE(TAG, "触摸不可用，按钮点不了");
    }

    /* 没插卡也让界面起来，点开始录音时会再试一次挂载 */
    sdcard_mount();

    ESP_ERROR_CHECK(mic_init());
    ESP_ERROR_CHECK(recorder_init());
    ui_init();

    /* 等第一帧画完再开背光，免得先闪一下白屏 */
    vTaskDelay(pdMS_TO_TICKS(150));
    display_set_brightness(BACKLIGHT_PERCENT);

    ESP_LOGI(TAG, "就绪");
}
