/*
 * Sensirion SHT40 / SHT41 / SHT45 温湿度传感器驱动。
 * 三个型号命令集完全一样，只有出厂精度不同，所以一份代码通吃。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/*
 * 传感器的电可以从 GPIO 直接取（amoled-clock 就是这样，图的是四根杜邦线能插在一起），
 * pwr_gpio < 0 表示接的是 3V3 电源轨，什么都不做。
 * 用 GPIO 供电时必须在 i2c_bus_init() 之前调用：总线上拉一直是带电的，传感器 VDD
 * 还没起来的那段时间里，上拉会通过它的 ESD 二极管倒灌进去给芯片寄生供电，属于
 * 超规格状态，越早拉高这个窗口越短。
 *
 * 不要顺手加"断电重启传感器"的逻辑：VDD 拉到 0 时 SDA/SCL 会被它的 ESD 二极管
 * 钳到 0.7V 左右，同一条总线上的触摸、IO 扩展会一起瘫掉。
 */
void sht4x_power_on(int pwr_gpio);

/* 复位并读序列号，确认传感器在线 */
esp_err_t sht4x_probe(void);

/* 高精度单次测量。约耗时 10ms（含等待），带 CRC 校验 */
esp_err_t sht4x_read(float *temperature_c, float *humidity_rh);
