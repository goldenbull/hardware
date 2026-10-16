/*
 * 声控开关屏：麦克风采集任务 → 音量触发 / 关键词识别 → 回调。
 * 用哪种在 menuconfig → 声控方式 里选。采集链路是 components/mic，
 * 和 kws-recorder 录训练数据用的完全是同一份。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    VOICE_SCREEN_ON = 0,    /* 关键词"打开屏幕" */
    VOICE_SCREEN_OFF,       /* 关键词"关闭屏幕" */
    VOICE_SCREEN_TOGGLE,    /* 音量触发：只知道"有响声"，切换开关 */
} voice_cmd_t;

typedef void (*voice_cb_t)(voice_cmd_t cmd);

/* 初始化麦克风（和识别器）并启动采集任务。I2C 必须已经初始化 */
esp_err_t voice_start(voice_cb_t on_cmd);

/* 采集任务在跑才是 true */
bool voice_is_running(void);

/* 上次调用以来麦克风的峰值绝对值（0~32767），读完清零。调试面板看麦克风有没有声音用 */
uint16_t voice_take_peak(void);

/*
 * 音量触发模式：上次调用以来平滑音量的最大值（dBFS，整数），读完清零；
 * 以及开机以来的触发次数（包括被冷却挡掉的）。调试面板用来调阈值。
 */
int voice_take_level_max_db(void);
uint32_t voice_trigger_count(void);
