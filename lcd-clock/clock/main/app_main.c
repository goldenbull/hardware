/*
 * Waveshare ESP32-C5-Touch-LCD-3.5 桌面时钟，逻辑照搬 amoled-clock：
 *
 *   - Wi-Fi + NTP 校时，显示日期、星期、时分秒
 *   - 外接 Sensirion SHT45（I2C 0x44）读温湿度。板载 SHTC3 被板子自身发热带偏，
 *     不用（见 ../sensor-test）
 *   - 温度 > 30°C 屏幕底部烧火，< 20°C 顶部飘雪
 *   - 单击屏幕开关显示，上下滑动调亮度
 *   - IMU 判断重力方向，屏幕倒过来放时自动翻转 180°
 *   - 比 amoled-clock 多一项：说"打开屏幕"/"关闭屏幕"开关屏（识别部分待实现，见 kws.c）
 *
 * 屏是 320x480 竖装的，这里用横屏 480x320。
 *
 * 任务划分（单核，优先级从高到低）：
 *   voice   —— 5，每 20ms 读一块麦克风数据喂给识别器
 *   gesture —— 3，20ms 轮询 FT6336，做手势判定
 *   lvgl    —— 2，跑 lv_timer_handler，界面的所有绘制都在这个任务里
 *   sensor  —— 2，2s 读一次 SHT45
 *   imu     —— 2，250ms 读一次加速度
 *   main    —— 1，起完上面几个之后盯着网络状态
 */
#include "display.h"
#include "esp_err.h"
#include "esp_log.h"
#include "exio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gesture.h"
#include "i2c_bus.h"
#include "imu.h"
#include "net_time.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "sht4x.h"
#include "touch.h"
#include "ui.h"
#include "voice.h"

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

/* 语音只管开或关，不是切换：屏已经是目标状态时什么都不做，误识别一次也不会把屏关掉又打开 */
static void on_keyword(kws_result_t result)
{
    const bool want_on = (result == KWS_SCREEN_ON);
    if (want_on != display_is_on()) {
        display_set_on(want_on);
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

    sht4x_power_on(CONFIG_APP_SHT4X_PWR_GPIO);  /* 必须排在 i2c_bus_init 之前，原因见 sht4x.h */
    ESP_ERROR_CHECK(i2c_bus_init());
    ESP_ERROR_CHECK(exio_init());               /* 放开屏和触摸的复位，必须在两者之前；功放保持关闭 */
    ESP_ERROR_CHECK(display_init_orientation(DISPLAY_LANDSCAPE));
    ui_init();

    /* 等第一帧画完再开背光，免得先闪一下白屏 */
    vTaskDelay(pdMS_TO_TICKS(150));
    display_set_brightness(CONFIG_APP_BRIGHTNESS_DEFAULT);

    const gesture_callbacks_t gesture_cb = {
        .on_brightness_change = ui_show_brightness,
        .on_brightness_end = ui_hide_brightness,
    };
    /* 触摸坏了时钟照样能用，只是不能点 */
    if (touch_init(false) != ESP_OK || gesture_start(&gesture_cb) != ESP_OK) {
        ESP_LOGW(TAG, "触摸不可用，单击开关屏和滑动调亮度将失效");
    }

#if CONFIG_APP_IMU_AUTO_FLIP
    /* IMU 不在也不影响看时间，只是不会自动翻转 */
    if (imu_init(display_set_flipped) != ESP_OK) {
        ESP_LOGW(TAG, "IMU 不可用，屏幕方向将固定不变");
    }
#endif

#if CONFIG_APP_VOICE_ENABLE
    if (voice_start(on_keyword) != ESP_OK) {
        ESP_LOGW(TAG, "麦克风不可用，语音开关屏将失效");
    }
#endif

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
