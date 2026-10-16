/*
 * 全局触摸手势：单击开关屏、上下滑动调亮度、左右滑动切换调试面板。
 * 前两个的逻辑和 amoled-clock 的 touch.c 一样，触摸芯片的读取放在共用组件
 * components/display/touch.c 里。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    /* 亮度调节过程中持续回调，用来显示屏上的亮度条 */
    void (*on_brightness_change)(uint8_t percent);
    /* 手指抬起、亮度调节结束 */
    void (*on_brightness_end)(void);
    /* 手指从右往左划 */
    void (*on_swipe_left)(void);
    /* 手指从左往右划 */
    void (*on_swipe_right)(void);
} gesture_callbacks_t;

/* 必须在 touch_init(false) 成功之后调用 */
esp_err_t gesture_start(const gesture_callbacks_t *callbacks);

/*
 * 最后一次检测到手指在屏上的时刻（esp_timer 毫秒，只用来算差值），从没摸过返回 0。
 * 声控用它避开手指敲屏的声音。
 */
uint32_t gesture_last_touch_ms(void);
