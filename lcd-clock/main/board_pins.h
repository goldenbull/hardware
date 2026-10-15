/*
 * Waveshare ESP32-C5-Touch-LCD-3.5 引脚定义
 * 数值来自官方示例包 example/ESP-IDF-V554 里的 BSP（esp32_c5_touch_lcd_3_5.h）。
 */
#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

/*
 * ---- SPI2：ST7796 屏 + TF 卡共用一条总线 ----
 *
 * SCK/MOSI 两者共用，MISO 只有 TF 卡用（屏是只写的）。两边靠各自的 CS 区分，
 * spi_master 驱动在设备之间自动做总线仲裁，所以屏刷新和写卡可以在不同任务里并发。
 * 总线由 display_init() 初始化，sdcard 模块只往上挂设备，因此必须先初始化显示。
 */
#define SPI_HOST_SHARED         SPI2_HOST
#define PIN_NUM_SPI_SCLK        GPIO_NUM_6
#define PIN_NUM_SPI_MOSI        GPIO_NUM_7
#define PIN_NUM_SPI_MISO        GPIO_NUM_2

#define PIN_NUM_LCD_CS          GPIO_NUM_8
#define PIN_NUM_LCD_DC          GPIO_NUM_5
/* 屏复位、背光都不在 GPIO 上，走 IO 扩展芯片，见下面 EXIO_* */
#define LCD_PIXEL_CLOCK_HZ      (60 * 1000 * 1000)
#define LCD_H_RES               320
#define LCD_V_RES               480
#define LCD_BIT_PER_PIXEL       16

/* 卡座焊在板背面，官方 BSP 提示 SD 不通时检查 JP8 第 2 位是否拨到 ON */
#define PIN_NUM_SD_CS           GPIO_NUM_9

/*
 * ---- I2C0：触摸 FT6336、ES8311、IO 扩展 CH32V003、SHTC3、QMI8658、PCF85063、AXP2101 ----
 * 官方 BSP 开内部上拉、跑 400kHz。
 */
#define I2C_PORT_NUM            0
#define PIN_NUM_I2C_SCL         GPIO_NUM_26
#define PIN_NUM_I2C_SDA         GPIO_NUM_27
#define I2C_CLK_SPEED_HZ        400000

#define I2C_ADDR_FT6336         0x38    /* 触摸 */
/* ES8311 的地址由 esp_codec_dev 的 ES8311_CODEC_DEFAULT_ADDR 给出，这里不重复定义 */

/*
 * ---- I2S0：ES8311 ----
 * 板上没接 MCLK，ES8311 用 BCLK 倍频出内部主时钟（esp_codec_dev 的 use_mclk=false）。
 */
#define PIN_NUM_I2S_BCLK        GPIO_NUM_23
#define PIN_NUM_I2S_WS          GPIO_NUM_10
#define PIN_NUM_I2S_DOUT        GPIO_NUM_25     /* ESP -> ES8311 DAC，本工程不放音 */
#define PIN_NUM_I2S_DIN         GPIO_NUM_24     /* ES8311 ADC -> ESP，麦克风 */
#define PIN_NUM_I2S_MCLK        GPIO_NUM_NC

/*
 * ---- CH32V003 IO 扩展 ----
 * 官方 BSP 的 bsp_io_expander_init() 用的就是这几位。
 */
#define EXIO_TOUCH_RST          0       /* 低有效 */
#define EXIO_LCD_RST            1       /* 低有效 */
#define EXIO_PA_EN              5       /* NS4150B 功放使能，高有效 */
