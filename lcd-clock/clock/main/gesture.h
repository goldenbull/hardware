/*
 * 全局触摸手势：单击开关屏、上下滑动调亮度。逻辑和 amoled-clock 的 touch.c 一样，
 * 触摸芯片的读取放在共用组件 components/display/touch.c 里。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    /* 亮度调节过程中持续回调，用来显示屏上的亮度条 */
    void (*on_brightness_change)(uint8_t percent);
    /* 手指抬起、亮度调节结束 */
    void (*on_brightness_end)(void);
} gesture_callbacks_t;

/* 必须在 touch_init(false) 成功之后调用 */
esp_err_t gesture_start(const gesture_callbacks_t *callbacks);
