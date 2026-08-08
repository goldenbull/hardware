#include "sht4x.h"

#include <inttypes.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

static const char *TAG = "sht4x";

#define SHT4X_CMD_MEASURE_HIGH_PRECISION 0xFD
#define SHT4X_CMD_READ_SERIAL            0x89
#define SHT4X_CMD_SOFT_RESET             0x94

/* 高精度测量典型 6.9ms、最大 8.2ms，留点余量 */
#define SHT4X_MEASURE_DELAY_MS 12

/* 供电拉高后等芯片起来，数据手册的上电时间是 1ms，给足余量 */
#define SHT4X_POWER_UP_MS 10

void sht4x_power_on(void)
{
    const gpio_config_t cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_NUM_SHT4X_PWR,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(PIN_NUM_SHT4X_PWR, 1);
    vTaskDelay(pdMS_TO_TICKS(SHT4X_POWER_UP_MS));

    ESP_LOGI(TAG, "SHT4x 供电已开（GPIO%d）", PIN_NUM_SHT4X_PWR);
}

/* CRC-8，多项式 0x31，初值 0xFF —— 数据手册规定 */
static uint8_t sht4x_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

esp_err_t sht4x_probe(void)
{
    const uint8_t reset = SHT4X_CMD_SOFT_RESET;
    esp_err_t err = i2c_bus_write(I2C_ADDR_SHT4X, &reset, 1);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(2));

    const uint8_t cmd = SHT4X_CMD_READ_SERIAL;
    err = i2c_bus_write(I2C_ADDR_SHT4X, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(2));

    uint8_t buf[6];
    err = i2c_bus_read(I2C_ADDR_SHT4X, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }
    if (sht4x_crc8(buf, 2) != buf[2] || sht4x_crc8(buf + 3, 2) != buf[5]) {
        ESP_LOGW(TAG, "序列号 CRC 校验失败，可能是接线太长或者上拉不足");
        return ESP_ERR_INVALID_CRC;
    }

    const uint32_t serial = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                            ((uint32_t)buf[3] << 8) | buf[4];
    ESP_LOGI(TAG, "SHT4x 在线，序列号 0x%08" PRIX32, serial);
    return ESP_OK;
}

esp_err_t sht4x_read(float *temperature_c, float *humidity_rh)
{
    const uint8_t cmd = SHT4X_CMD_MEASURE_HIGH_PRECISION;
    esp_err_t err = i2c_bus_write(I2C_ADDR_SHT4X, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(SHT4X_MEASURE_DELAY_MS));

    uint8_t buf[6];
    err = i2c_bus_read(I2C_ADDR_SHT4X, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }
    if (sht4x_crc8(buf, 2) != buf[2] || sht4x_crc8(buf + 3, 2) != buf[5]) {
        return ESP_ERR_INVALID_CRC;
    }

    const uint16_t raw_t = ((uint16_t)buf[0] << 8) | buf[1];
    const uint16_t raw_rh = ((uint16_t)buf[3] << 8) | buf[4];

    *temperature_c = -45.0f + 175.0f * (float)raw_t / 65535.0f;

    /* 湿度公式本身会略微超出 0~100，手册要求截断 */
    float rh = -6.0f + 125.0f * (float)raw_rh / 65535.0f;
    if (rh < 0.0f) {
        rh = 0.0f;
    } else if (rh > 100.0f) {
        rh = 100.0f;
    }
    *humidity_rh = rh;

    return ESP_OK;
}
