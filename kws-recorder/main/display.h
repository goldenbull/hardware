/*
 * ST7796 3.5 寸 SPI 屏（320x480 竖屏）+ LVGL 8。
 *
 * display_init() 同时初始化屏和 TF 卡共用的 SPI2 总线，所以必须排在 sdcard_mount() 前面；
 * 屏复位走 IO 扩展，所以又必须排在 exio_init() 后面。背光不在这里开（见 app_main），
 * 免得点亮时看到的是还没画界面的白屏。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t display_init(void);

/* LVGL 不是线程安全的，LVGL 任务以外的地方调 lv_* 之前必须先拿这把锁。timeout_ms<0 表示一直等 */
bool display_lock(int timeout_ms);
void display_unlock(void);
