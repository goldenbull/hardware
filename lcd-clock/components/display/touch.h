/*
 * FT6336 电容触摸。
 *
 * 两种用法：
 *   - kws-recorder 界面上有按钮，接成 LVGL 指针设备（lvgl_indev=true）
 *   - 时钟只做全局手势，自己轮询 touch_read_point()，关屏状态下也能用（lvgl_indev=false）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* 必须在 display_init*() 之后调用 */
esp_err_t touch_init(bool lvgl_indev);

/* 读一个触点，有触摸返回 true。坐标已换算成当前屏幕方向（含 180° 翻转）下的 LVGL 坐标 */
bool touch_read_point(uint16_t *x, uint16_t *y);
