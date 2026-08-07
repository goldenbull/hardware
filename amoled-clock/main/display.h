/*
 * SH8601 AMOLED + LVGL 显示层。
 *
 * 关键点：LVGL 配了两块整屏大小的绘制缓冲（放在 PSRAM）并打开 full_refresh，
 * 每一帧都是在内存里画完整屏后一次性 DMA 给屏幕，屏幕上不存在"先擦后画"的
 * 中间状态，所以不会闪。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* AMOLED 没有背光，亮度是屏内寄存器 0x51，0 最暗、255 最亮 */
#define DISPLAY_BRIGHTNESS_MIN     8
#define DISPLAY_BRIGHTNESS_MAX     255
#define DISPLAY_BRIGHTNESS_DEFAULT 180

esp_err_t display_init(void);

/*
 * LVGL 的 API 不是线程安全的。除了 LVGL 自己的 timer 回调（已经在 LVGL
 * 任务里跑）以外，任何线程碰 lv_* 之前都要先拿这把锁。
 */
bool display_lock(int timeout_ms);
void display_unlock(void);

void display_set_brightness(uint8_t brightness);
uint8_t display_get_brightness(void);

void display_set_on(bool on);
bool display_is_on(void);
