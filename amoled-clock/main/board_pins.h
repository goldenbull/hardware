/*
 * Waveshare ESP32-S3-Touch-AMOLED-1.91 引脚定义
 * 数值来自官方示例包 02_Example/ESP-IDF/07_FactoryProgram。
 */
#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

/* ---- AMOLED: SH8601, 536x240, QSPI ---- */
#define LCD_HOST                SPI2_HOST
#define LCD_H_RES               536
#define LCD_V_RES               240
#define LCD_BIT_PER_PIXEL       16

#define PIN_NUM_LCD_CS          GPIO_NUM_6
#define PIN_NUM_LCD_PCLK        GPIO_NUM_47
#define PIN_NUM_LCD_DATA0       GPIO_NUM_18
#define PIN_NUM_LCD_DATA1       GPIO_NUM_7
#define PIN_NUM_LCD_DATA2       GPIO_NUM_48
#define PIN_NUM_LCD_DATA3       GPIO_NUM_5
#define PIN_NUM_LCD_RST         GPIO_NUM_17

/*
 * ---- I2C0：板载触摸 + IMU，外接 SHT45 也挂这条总线 ----
 *
 * SHT45 接线（按卖家给的线色）：
 *   红 -> 3V3     黑 -> GND
 *   黄 -> SDA (GPIO40)
 *   绿 -> SCL (GPIO39)
 * 屏蔽层接 GND。板上已有上拉电阻，传感器侧不需要再加。
 *
 * 官方示例跑 300kHz；这里降到 200kHz，因为 SHT45 是带线引出的，
 * 线长会拉高总线电容。触摸和 IMU 在 200kHz 下工作正常。
 * 如果线特别长（>50cm）读数不稳，继续降到 100000。
 */
#define I2C_PORT_NUM            0
#define PIN_NUM_I2C_SCL         GPIO_NUM_39
#define PIN_NUM_I2C_SDA         GPIO_NUM_40
#define I2C_CLK_SPEED_HZ        200000

#define I2C_ADDR_FT3168         0x38    /* 触摸 */
#define I2C_ADDR_QMI8658        0x6B    /* IMU，本工程未使用 */
#define I2C_ADDR_SHT4X          0x44    /* SHT40/41/45 出厂默认地址 */
