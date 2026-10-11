/*
 * TF 卡，SPI 模式，挂在和屏共用的 SPI2 总线上（总线由 display_init() 初始化）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SDCARD_MOUNT_POINT "/sdcard"

/* 已经挂上就直接返回 ESP_OK，所以开机没插卡、后来插上的情况可以再调一次 */
esp_err_t sdcard_mount(void);

bool sdcard_is_mounted(void);

/* 剩余空间（字节）。没挂上返回 0 */
uint64_t sdcard_free_bytes(void);
