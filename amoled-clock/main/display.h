/*
 * SH8601 AMOLED + LVGL 显示层。
 *
 * 关键点：LVGL 配了两块分块绘制缓冲，放在内部 DMA RAM 而不是 PSRAM。
 * esp_lcd 的 SPI panel IO 不会给事务设置 SPI_TRANS_DMA_USE_PSRAM，PSRAM 里的
 * 缓冲会逼着 spi_master 另外申请一块等长的内部临时缓冲，整屏尺寸下必然失败。
 * 详见 display.c 里 LVGL_BUF_LINES 的注释。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * AMOLED 没有背光，亮度是屏内寄存器 0x51，0 最暗、255 最亮。
 * MIN 是"滑到最左"的下限，不是 0——最暗档也要看得见，
 * 真正的全黑只留给关屏（display_set_on(false) 会单独写 0）。
 */
#define DISPLAY_BRIGHTNESS_MIN     60
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
