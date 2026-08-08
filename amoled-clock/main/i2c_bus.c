#include "i2c_bus.h"

#include "board_pins.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "i2c_bus";

#define I2C_TIMEOUT_MS 100

/* 这条总线上的器件：FT3168 触摸、QMI8658 IMU、SHT45，留一个余量 */
#define I2C_MAX_DEVICES 4

static i2c_master_bus_handle_t s_bus;

/*
 * 新版 i2c_master 驱动是按"设备句柄"操作的，但上层调用方（touch / sht4x）
 * 用地址来标识器件更直观，所以这里按需创建句柄并缓存起来。
 */
static struct {
    uint8_t                addr;
    i2c_master_dev_handle_t dev;
} s_devices[I2C_MAX_DEVICES];
static size_t s_device_count;

static SemaphoreHandle_t s_lock;

static esp_err_t get_device(uint8_t addr, i2c_master_dev_handle_t *out)
{
    for (size_t i = 0; i < s_device_count; i++) {
        if (s_devices[i].addr == addr) {
            *out = s_devices[i].dev;
            return ESP_OK;
        }
    }

    if (s_device_count >= I2C_MAX_DEVICES) {
        return ESP_ERR_NO_MEM;
    }

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_CLK_SPEED_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &cfg, &dev), TAG,
                        "添加 I2C 设备 0x%02X 失败", addr);

    s_devices[s_device_count].addr = addr;
    s_devices[s_device_count].dev = dev;
    s_device_count++;

    *out = dev;
    return ESP_OK;
}

esp_err_t i2c_bus_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT_NUM,
        .sda_io_num = PIN_NUM_I2C_SDA,
        .scl_io_num = PIN_NUM_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c_new_master_bus failed");

    ESP_LOGI(TAG, "I2C%d 就绪：SCL=%d SDA=%d %dHz",
             I2C_PORT_NUM, PIN_NUM_I2C_SCL, PIN_NUM_I2C_SDA, I2C_CLK_SPEED_HZ);
    return ESP_OK;
}

esp_err_t i2c_bus_read_reg(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = get_device(addr, &dev);
    if (err == ESP_OK) {
        err = i2c_master_transmit_receive(dev, &reg, 1, buf, len, I2C_TIMEOUT_MS);
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t i2c_bus_write_reg(uint8_t addr, uint8_t reg, const uint8_t *buf, size_t len)
{
    uint8_t tmp[16];
    if (len + 1 > sizeof(tmp)) {
        return ESP_ERR_INVALID_SIZE;
    }
    tmp[0] = reg;
    for (size_t i = 0; i < len; i++) {
        tmp[i + 1] = buf[i];
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = get_device(addr, &dev);
    if (err == ESP_OK) {
        err = i2c_master_transmit(dev, tmp, len + 1, I2C_TIMEOUT_MS);
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t i2c_bus_write(uint8_t addr, const uint8_t *buf, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = get_device(addr, &dev);
    if (err == ESP_OK) {
        err = i2c_master_transmit(dev, buf, len, I2C_TIMEOUT_MS);
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t i2c_bus_read(uint8_t addr, uint8_t *buf, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = get_device(addr, &dev);
    if (err == ESP_OK) {
        err = i2c_master_receive(dev, buf, len, I2C_TIMEOUT_MS);
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t i2c_bus_probe(uint8_t addr)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = i2c_master_probe(s_bus, addr, I2C_TIMEOUT_MS);
    xSemaphoreGive(s_lock);
    return err;
}
