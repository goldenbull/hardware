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
/*
 * 屏幕没有硬件复位线：官方引脚分配表里 AMOLED 一列只有 DB0/DB1/CS/DCX_RS/
 * SDO/TE/RD_SDI/WRX_SCL，没有 RST。官方例程写的 GPIO17 其实驱动的是一个没接
 * 到屏幕的脚（GPIO17 在表里属于引到排针的空闲 IO）。填 -1 让驱动改走软复位
 * （SWRESET 0x01 + 80ms，见 esp_lcd_sh8601.c:185），同时把 GPIO17 空出来给
 * 下面的传感器 I2C 用。
 */
#define PIN_NUM_LCD_RST         (-1)

/*
 * ---- I2C0：板载触摸 + IMU，外接 SHT45 也挂这条总线 ----
 *
 * 触摸和 IMU 在 PCB 上焊死在 GPIO39/40（官方表里 TP_SCL/TP_SDA 和
 * IMU_SCL/IMU_SDA 就是这两个脚），所以这条总线的引脚改不得。
 * 板上已有上拉电阻，传感器侧不需要再加。
 *
 * SHT45 插在排针左列连续的四个脚上，四个杜邦母口可以并排：
 *   29 GP40 -> 黄 SDA
 *   28 GND  -> 黑
 *   27 GP39 -> 绿 SCL
 *   26 GP38 -> 红 3V3（不是电源轨，是下面那个当电源用的 GPIO）
 *
 * 官方示例跑 300kHz；这里降到 200kHz，因为 SHT45 是带线引出的，
 * 线长会拉高总线电容。触摸和 IMU 在 200kHz 下工作正常。
 * 如果线特别长（>50cm）读数不稳，继续降到 100000。
 */
#define I2C_PORT_NUM            0
#define PIN_NUM_I2C_SCL         GPIO_NUM_39
#define PIN_NUM_I2C_SDA         GPIO_NUM_40
#define I2C_CLK_SPEED_HZ        200000

/*
 * 用 GPIO38 直接给 SHT45 供电，图的是它紧挨着 SCL(27)，四根线能插在一起。
 * SHT45 待机 0.4µA、测量峰值约 500µA，而 S3 单脚驱动能力约 20mA，绰绰有余；
 * 500µA 在输出管上的压降只有几十毫伏，传感器看到的仍是 3.2V 出头。
 * GPIO38 不是 strapping 脚（S3 的是 0/3/45/46），启动时不会被拉到固定电平。
 *
 * 注意不要顺手加"断电重启传感器"的逻辑：SHT45 和触摸/IMU 共用这条总线，
 * 把它的 VDD 拉到 0 时，SDA/SCL 会通过它的 ESD 二极管被钳到 0.7V 左右，
 * 整条总线跟着瘫掉，触摸也就一起读不出来了。所以这个脚只在启动时拉高一次
 * 就一直保持，不做上下电。
 */
#define PIN_NUM_SHT4X_PWR       GPIO_NUM_38

#define I2C_ADDR_FT3168         0x38    /* 触摸 */
#define I2C_ADDR_QMI8658        0x6B    /* IMU，本工程未使用 */
#define I2C_ADDR_SHT4X          0x44    /* SHT40/41/45 出厂默认地址 */
