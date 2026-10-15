#include "shtc3.h"

#include <stdbool.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

static const char *TAG = "shtc3";

#define CMD_WAKEUP      0x3517
#define CMD_SLEEP       0xB098
#define CMD_SOFT_RESET  0x805D
#define CMD_READ_ID     0xEFC8

/* 唤醒最长 240µs，软复位最长 240µs */
#define WAKEUP_MS       1
/* 正常模式最长 12.1ms，低功耗模式最长 0.8ms。还没测完时读会被 NACK，所以再重试几次 */
#define MEASURE_WAIT_MS 13
#define MEASURE_RETRIES 5

static const struct {
    uint16_t cmd;
    bool     t_first;
    const char *name;
} s_modes[SHTC3_MODE_COUNT] = {
    [SHTC3_MODE_NORMAL_T_FIRST]  = { 0x7866, true,  "正常/T先"  },
    [SHTC3_MODE_NORMAL_RH_FIRST] = { 0x58E0, false, "正常/RH先" },
    [SHTC3_MODE_LP_T_FIRST]      = { 0x609C, true,  "低功耗/T先" },
    [SHTC3_MODE_LP_RH_FIRST]     = { 0x401A, false, "低功耗/RH先" },
};

const char *shtc3_mode_name(shtc3_mode_t mode)
{
    return s_modes[mode].name;
}

/* CRC-8，多项式 0x31，初值 0xFF */
static uint8_t crc8(const uint8_t *data, size_t len)
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

static esp_err_t send_cmd(uint16_t cmd)
{
    const uint8_t buf[2] = { cmd >> 8, cmd & 0xFF };
    return i2c_bus_write(SHTC3_ADDR, buf, sizeof(buf));
}

static esp_err_t wakeup(void)
{
    esp_err_t err = send_cmd(CMD_WAKEUP);
    vTaskDelay(pdMS_TO_TICKS(WAKEUP_MS));
    return err;
}

float shtc3_raw_to_celsius(uint16_t raw)
{
    return -45.0f + 175.0f * (float)raw / 65536.0f;
}

esp_err_t shtc3_probe(uint16_t *id)
{
    /* 刚上电时传感器在空闲态，唤醒命令可能不被 ACK，不当错误 */
    wakeup();

    esp_err_t err = send_cmd(CMD_SOFT_RESET);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(WAKEUP_MS));

    err = send_cmd(CMD_READ_ID);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t buf[3];
    err = i2c_bus_read(SHTC3_ADDR, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }
    if (crc8(buf, 2) != buf[2]) {
        return ESP_ERR_INVALID_CRC;
    }
    *id = ((uint16_t)buf[0] << 8) | buf[1];

    /* 手册：ID 的 bit11 和 bit5..0 固定为 1 和 000111，其余位是厂家保留 */
    if ((*id & 0x083F) != 0x0807) {
        ESP_LOGW(TAG, "ID 0x%04X 不像 SHTC3", *id);
    }

    send_cmd(CMD_SLEEP);
    return ESP_OK;
}

esp_err_t shtc3_measure(shtc3_mode_t mode, shtc3_sample_t *out)
{
    esp_err_t err = wakeup();
    if (err != ESP_OK) {
        return err;
    }

    err = send_cmd(s_modes[mode].cmd);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t buf[6];
    for (int i = 0; i < MEASURE_RETRIES; i++) {
        vTaskDelay(pdMS_TO_TICKS(i == 0 ? MEASURE_WAIT_MS : 2));
        err = i2c_bus_read(SHTC3_ADDR, buf, sizeof(buf));
        if (err == ESP_OK) {
            break;
        }
    }
    send_cmd(CMD_SLEEP);
    if (err != ESP_OK) {
        return err;
    }
    if (crc8(buf, 2) != buf[2] || crc8(buf + 3, 2) != buf[5]) {
        return ESP_ERR_INVALID_CRC;
    }

    const uint16_t first = ((uint16_t)buf[0] << 8) | buf[1];
    const uint16_t second = ((uint16_t)buf[3] << 8) | buf[4];
    out->raw_t = s_modes[mode].t_first ? first : second;
    out->raw_rh = s_modes[mode].t_first ? second : first;
    out->temperature_c = shtc3_raw_to_celsius(out->raw_t);
    out->humidity_rh = 100.0f * (float)out->raw_rh / 65536.0f;
    return ESP_OK;
}
