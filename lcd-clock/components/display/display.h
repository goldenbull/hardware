/*
 * ST7796 3.5 寸 SPI 屏（物理 320x480）+ LVGL 8。
 *
 * display_init() 同时初始化屏和 TF 卡共用的 SPI2 总线，所以必须排在 sdcard_mount() 前面；
 * 屏复位走 IO 扩展，所以又必须排在 exio_init() 后面。背光不在这里开（见 app_main），
 * 免得点亮时看到的是还没画界面的白屏。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    DISPLAY_PORTRAIT,       /* 320x480，kws-recorder 用 */
    DISPLAY_LANDSCAPE,      /* 480x320，时钟用 */
} display_orientation_t;

/* 背光百分比。MIN 是滑动调节的下限，真正的全黑只留给关屏 */
#define DISPLAY_BRIGHTNESS_MIN     5
#define DISPLAY_BRIGHTNESS_MAX     100

/* 竖屏，等同 display_init_orientation(DISPLAY_PORTRAIT) */
esp_err_t display_init(void);
esp_err_t display_init_orientation(display_orientation_t orientation);

/* 当前方向下 LVGL 看到的分辨率 */
int display_width(void);
int display_height(void);

/* LVGL 不是线程安全的，LVGL 任务以外的地方调 lv_* 之前必须先拿这把锁。timeout_ms<0 表示一直等 */
bool display_lock(int timeout_ms);
void display_unlock(void);

/*
 * 背光亮度 0~100%，走 CH32V003 的 PWM。关屏状态下只记住数值，开屏时再生效。
 * 初始值是 0（display_init 不开背光），调用方画完第一帧后再设。
 */
void display_set_brightness(uint8_t percent);
uint8_t display_get_brightness(void);

/* 开关屏：背光 + DISPON/DISPOFF。关屏时界面应跳过重绘 */
void display_set_on(bool on);
bool display_is_on(void);

/* 上一次 display_set_on() 完成的时刻（esp_timer 毫秒，32 位会回绕，只用来算差值）。从没调过返回 0 */
uint32_t display_last_switch_ms(void);

/*
 * 整屏转 180°，靠改 MADCTL 实现，不走 LVGL 软件旋转。
 * touch_read_point() 的坐标会自动跟着换算。
 */
void display_set_flipped(bool flipped);
bool display_is_flipped(void);
