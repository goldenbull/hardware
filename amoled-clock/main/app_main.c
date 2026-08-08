/*
 * Waveshare ESP32-S3-Touch-AMOLED-1.91 桌面时钟
 *
 *   - Wi-Fi + NTP 校时，显示日期、星期、时分秒
 *   - 外接 Sensirion SHT45（I2C 0x44）读温湿度，显示在时钟下方
 *   - 温度 > 30°C 屏幕底部烧火，< 20°C 顶部飘雪
 *   - 单击屏幕开关显示，横向滑动调亮度
 *
 * 任务划分：
 *   lvgl   —— 跑 lv_timer_handler，界面的所有绘制都在这个任务里
 *   touch  —— 20ms 轮询 FT3168，做手势判定
 *   sensor —— 2s 读一次 SHT45
 *   main   —— 起完上面几个之后盯着网络状态
 */
#include "display.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "imu.h"
#include "net_time.h"
#include "nvs_flash.h"
#include "sht4x.h"
#include "touch.h"
#include "ui.h"

static const char *TAG = "app";

#define SENSOR_PERIOD_MS       2000
#define SENSOR_RETRY_PERIOD_MS 5000
#define SENSOR_MAX_FAILS       3

static void sensor_task(void *arg)
{
    bool present = false;
    int  fails = 0;

    while (1) {
        if (!present) {
            if (sht4x_probe() == ESP_OK) {
                present = true;
                fails = 0;
            } else {
                /* 传感器可以随时插拔，没插就慢速重试，不影响时钟 */
                ui_set_environment(false, 0.0f, 0.0f);
                vTaskDelay(pdMS_TO_TICKS(SENSOR_RETRY_PERIOD_MS));
                continue;
            }
        }

        float temperature = 0.0f;
        float humidity = 0.0f;
        const esp_err_t err = sht4x_read(&temperature, &humidity);
        if (err == ESP_OK) {
            fails = 0;
            ui_set_environment(true, temperature, humidity);
        } else if (++fails >= SENSOR_MAX_FAILS) {
            ESP_LOGW(TAG, "SHT45 连续 %d 次读取失败（%s），标记为掉线", fails, esp_err_to_name(err));
            present = false;
            ui_set_environment(false, 0.0f, 0.0f);
        }

        vTaskDelay(pdMS_TO_TICKS(SENSOR_PERIOD_MS));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    sht4x_power_on();   /* 必须排在 i2c_bus_init 之前，原因见 sht4x.h */
    ESP_ERROR_CHECK(i2c_bus_init());
    ESP_ERROR_CHECK(display_init());
    ui_init();

    display_set_brightness(DISPLAY_BRIGHTNESS_DEFAULT);

    const touch_callbacks_t touch_cb = {
        .on_brightness_change = ui_show_brightness,
        .on_brightness_end = ui_hide_brightness,
    };
    /* 不带触摸的板子在这里会失败，让它继续跑，时钟照样能用 */
    if (touch_init(&touch_cb) != ESP_OK) {
        ESP_LOGW(TAG, "触摸不可用，双击开关屏和滑动调亮度将失效");
    }

    /* IMU 不在也不影响看时间，只是不会自动翻转 */
    if (imu_init(display_set_flipped) != ESP_OK) {
        ESP_LOGW(TAG, "IMU 不可用，屏幕方向将固定不变");
    }

    ESP_ERROR_CHECK(net_time_start());

    xTaskCreate(sensor_task, "sensor", 4096, NULL, 2, NULL);

    int last_state = -1;
    while (1) {
        const net_state_t state = net_time_get_state();
        if ((int)state != last_state) {
            last_state = (int)state;
            ui_set_net_state(state);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
