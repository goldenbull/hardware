/*
 * FT6336 电容触摸，接成 LVGL 的指针输入设备，界面上的按钮靠它点。
 *
 * 和官方 BSP 一致：屏在 display.c 里做了 X 镜像之后，触摸芯片报的原始坐标
 * 正好就是 320x480 竖屏坐标，不需要再交换或翻转。
 */
#pragma once

#include "esp_err.h"

/* 必须在 display_init() 之后调用（要用 LVGL） */
esp_err_t touch_init(void);
