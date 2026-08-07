/* 板上 I2C0 总线，触摸 / IMU / 外接 SHT45 共用 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t i2c_bus_init(void);

/* 先写 reg 再连读 len 字节 */
esp_err_t i2c_bus_read_reg(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len);

/* 写 reg + len 字节数据 */
esp_err_t i2c_bus_write_reg(uint8_t addr, uint8_t reg, const uint8_t *buf, size_t len);

/* 裸写 / 裸读，SHT4x 这类没有寄存器地址、只吃命令字节的器件用 */
esp_err_t i2c_bus_write(uint8_t addr, const uint8_t *buf, size_t len);
esp_err_t i2c_bus_read(uint8_t addr, uint8_t *buf, size_t len);

/* 探测某个地址是否 ACK */
esp_err_t i2c_bus_probe(uint8_t addr);
