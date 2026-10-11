/* 板上 I2C0 总线，触摸 / ES8311 / IO 扩展等共用 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t i2c_bus_init(void);

/* 先写 reg 再连读 len 字节 */
esp_err_t i2c_bus_read_reg(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len);

/* 写 reg + len 字节数据 */
esp_err_t i2c_bus_write_reg(uint8_t addr, uint8_t reg, const uint8_t *buf, size_t len);

/* 裸写 / 裸读，给没有寄存器地址、只吃命令字节的器件（如 SHTC3）预留 */
esp_err_t i2c_bus_write(uint8_t addr, const uint8_t *buf, size_t len);
esp_err_t i2c_bus_read(uint8_t addr, uint8_t *buf, size_t len);

/*
 * 总线句柄。esp_codec_dev（ES8311）和 CH32V003 IO 扩展组件只认句柄、自己往总线上
 * 挂设备，不经过上面这几个按地址读写的函数。i2c_master 驱动本身在总线级别加了锁，
 * 两条路径并发访问是安全的。
 */
i2c_master_bus_handle_t i2c_bus_get_handle(void);

/* 探测某个地址是否 ACK */
esp_err_t i2c_bus_probe(uint8_t addr);
