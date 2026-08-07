/*
 * FT3168 电容触摸 + 手势识别。
 *
 * 这里不接 LVGL 的 indev：界面上没有需要点击的控件，触摸只用来做两件
 * 全局手势——单击开关屏、横向滑动调亮度。自己轮询能在关屏状态下继续
 * 工作，也方便做连续调节。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    /* 亮度调节过程中持续回调，用来显示屏上的亮度条 */
    void (*on_brightness_change)(uint8_t brightness);
    /* 手指抬起、亮度调节结束 */
    void (*on_brightness_end)(void);
} touch_callbacks_t;

esp_err_t touch_init(const touch_callbacks_t *callbacks);

/* 读一个触点，有触摸返回 true。坐标已按横屏方向校正 */
bool touch_read_point(uint16_t *x, uint16_t *y);
