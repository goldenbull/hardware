/*
 * Sensirion SHT40 / SHT41 / SHT45 温湿度传感器驱动。
 * 三个型号命令集完全一样，只有出厂精度不同，所以一份代码通吃。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/*
 * 传感器的电是从 GPIO 直接取的（见 board_pins.h 的 PIN_NUM_SHT4X_PWR）。
 * 必须在 i2c_bus_init() 之前调用：总线上拉一直是带电的，传感器 VDD 还没起来
 * 的那段时间里，上拉会通过它的 ESD 二极管倒灌进去给芯片寄生供电，属于超规格
 * 状态，越早拉高这个窗口越短。
 */
void sht4x_power_on(void);

/* 复位并读序列号，确认传感器在线 */
esp_err_t sht4x_probe(void);

/* 高精度单次测量。约耗时 10ms（含等待），带 CRC 校验 */
esp_err_t sht4x_read(float *temperature_c, float *humidity_rh);
