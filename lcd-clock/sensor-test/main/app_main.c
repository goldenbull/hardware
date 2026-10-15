/*
 * SHTC3 读数验证
 *
 * 板载 SHTC3 在室温 23°C 时报 37°C。可能的原因分两类，这个程序把它们分开：
 *   1. 软件读错：命令/字节顺序/公式不对。每轮用四种模式各测一次并打印原始值，
 *      四种模式结果应该一致；另外打印"把湿度字当温度算"的值，如果它和 37°C 对得上，
 *      就是原来的驱动把 RH 先出的数据当成了温度。
 *   2. 传感器读得没错，是板子在发热：同时打印 ESP32-C5 片内温度和 AXP2101 充电状态，
 *      看 SHTC3 是否跟着芯片温度一起爬升。
 *
 * 只初始化 I2C 和 IO 扩展，屏、Wi-Fi、音频都不开，背光默认关，把板上热源压到最低。
 * 最有说服力的测法：断电放凉 10 分钟，再上电看第一轮读数和之后几分钟的曲线。
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include "board_pins.h"
#include "driver/temperature_sensor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "exio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "sdkconfig.h"
#include "shtc3.h"

static const char *TAG = "app";

#define AXP2101_ADDR     0x34
#define AXP2101_STATUS1  0x00   /* bit5 VBUS 正常，bit3 电池在位 */
#define AXP2101_STATUS2  0x01   /* bit6..5 电池电流方向，bit2..0 充电阶段 */

static temperature_sensor_handle_t s_tsens;
static bool s_has_axp;

static void scan_bus(void)
{
    ESP_LOGI(TAG, "I2C 扫描：");
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_bus_probe(addr) == ESP_OK) {
            ESP_LOGI(TAG, "  0x%02X", addr);
        }
    }
}

static void chip_temp_init(void)
{
    const temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&cfg, &s_tsens) != ESP_OK ||
        temperature_sensor_enable(s_tsens) != ESP_OK) {
        ESP_LOGW(TAG, "片内温度传感器不可用");
        s_tsens = NULL;
    }
}

static float chip_temp(void)
{
    float t = 0.0f;
    if (s_tsens == NULL || temperature_sensor_get_celsius(s_tsens, &t) != ESP_OK) {
        return -99.0f;
    }
    return t;
}

/* 充电时 AXP2101 会发热，它离 SHTC3 多远原理图上看不出来，所以把状态一并记下 */
static const char *axp_status(void)
{
    static const char *stages[] = { "涓流", "预充", "恒流", "恒压", "充满", "未充", "?", "?" };
    static char text[48];

    uint8_t s1, s2;
    if (!s_has_axp ||
        i2c_bus_read_reg(AXP2101_ADDR, AXP2101_STATUS1, &s1, 1) != ESP_OK ||
        i2c_bus_read_reg(AXP2101_ADDR, AXP2101_STATUS2, &s2, 1) != ESP_OK) {
        return "PMU -";
    }
    const uint8_t dir = (s2 >> 5) & 0x03;
    snprintf(text, sizeof(text), "VBUS=%d 电池=%s %s",
             (s1 >> 5) & 1,
             (s1 & (1 << 3)) ? "有" : "无",
             dir == 1 ? stages[s2 & 0x07] : (dir == 2 ? "放电" : "待机"));
    return text;
}

void app_main(void)
{
    ESP_ERROR_CHECK(i2c_bus_init());
    ESP_ERROR_CHECK(exio_init());
    exio_set_backlight(CONFIG_SHTC3_TEST_BACKLIGHT);
    chip_temp_init();

    scan_bus();
    s_has_axp = (i2c_bus_probe(AXP2101_ADDR) == ESP_OK);

    uint16_t id = 0;
    esp_err_t err = shtc3_probe(&id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SHTC3 (0x%02X) 无响应：%s", SHTC3_ADDR, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "SHTC3 ID=0x%04X，背光 %d%%，间隔 %dms",
             id, CONFIG_SHTC3_TEST_BACKLIGHT, CONFIG_SHTC3_TEST_INTERVAL_MS);
    ESP_LOGI(TAG, "列：秒 | 四种模式 温度/湿度 | 正常/T先 的原始值 | 湿度字当温度算 | 片内温度 | 电源");

    while (true) {
        const uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000);
        shtc3_sample_t s[SHTC3_MODE_COUNT];
        char line[160];
        int n = snprintf(line, sizeof(line), "%5" PRIu32 "s |", sec);

        for (int m = 0; m < SHTC3_MODE_COUNT; m++) {
            err = shtc3_measure(m, &s[m]);
            if (err == ESP_OK) {
                n += snprintf(line + n, sizeof(line) - n, " %5.2f/%4.1f%%",
                              s[m].temperature_c, s[m].humidity_rh);
            } else {
                n += snprintf(line + n, sizeof(line) - n, " %-11s", esp_err_to_name(err));
                s[m].raw_t = s[m].raw_rh = 0;
            }
        }

        const shtc3_sample_t *ref = &s[SHTC3_MODE_NORMAL_T_FIRST];
        snprintf(line + n, sizeof(line) - n, " | T=%04X RH=%04X | %5.2f | 片内 %4.1f",
                 ref->raw_t, ref->raw_rh, shtc3_raw_to_celsius(ref->raw_rh), chip_temp());
        ESP_LOGI(TAG, "%s | %s", line, axp_status());

        vTaskDelay(pdMS_TO_TICKS(CONFIG_SHTC3_TEST_INTERVAL_MS));
    }
}
