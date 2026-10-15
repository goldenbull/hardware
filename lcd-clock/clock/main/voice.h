/*
 * 语音开关屏：麦克风采集任务 → kws 识别 → 回调。
 * 采集链路是 components/mic，和 kws-recorder 录训练数据用的完全是同一份。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "kws.h"

typedef void (*voice_cb_t)(kws_result_t result);

/* 初始化麦克风和识别器并启动采集任务。I2C 必须已经初始化 */
esp_err_t voice_start(voice_cb_t on_keyword);

/* 采集任务在跑才是 true */
bool voice_is_running(void);

/* 上次调用以来麦克风的峰值绝对值（0~32767），读完清零。调试面板看麦克风有没有声音用 */
uint16_t voice_take_peak(void);
