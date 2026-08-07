/*
 * Sensirion SHT40 / SHT41 / SHT45 温湿度传感器驱动。
 * 三个型号命令集完全一样，只有出厂精度不同，所以一份代码通吃。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* 复位并读序列号，确认传感器在线 */
esp_err_t sht4x_probe(void);

/* 高精度单次测量。约耗时 10ms（含等待），带 CRC 校验 */
esp_err_t sht4x_read(float *temperature_c, float *humidity_rh);
