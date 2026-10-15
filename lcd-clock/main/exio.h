/*
 * 板上的 CH32V003 IO 扩展。屏复位、触摸复位、功放使能、背光 PWM 都在它上面，
 * 所以它必须是 I2C 之后第一个初始化的东西：不放开复位，屏和触摸都不会响应。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* 拉一次屏和触摸的复位，功放保持关闭，背光先灭着 */
esp_err_t exio_init(void);

/* 背光 0~100% */
esp_err_t exio_set_backlight(uint8_t percent);

/* NS4150B 功放。录音时必须关着，否则功放底噪会经喇叭耦合进麦克风 */
esp_err_t exio_set_pa(bool on);
