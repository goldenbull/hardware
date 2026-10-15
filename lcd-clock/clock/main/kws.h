/*
 * 离线关键词识别："打开屏幕" / "关闭屏幕"。
 *
 * ===== 还没实现 =====
 * 模型用 kws-recorder 采的数据训练，接口先定下来：voice.c 每 20ms 喂一块 16kHz/16bit
 * 单声道 PCM 进来，识别到关键词时返回对应的结果。模型接上以后只需要改 kws.c，
 * voice.c 和 app_main.c 不用动。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    KWS_NONE = 0,
    KWS_SCREEN_ON,      /* "打开屏幕" */
    KWS_SCREEN_OFF,     /* "关闭屏幕" */
} kws_result_t;

/* 加载模型、分配特征缓冲。在采集开始之前调用一次 */
esp_err_t kws_init(void);

/*
 * 喂一块音频，返回这一块结束时的识别结果。同一句话只应返回一次关键词，
 * 去抖（连续几帧都过阈值才算、触发后静默一段时间）由 kws.c 自己负责。
 */
kws_result_t kws_feed(const int16_t *pcm, size_t samples);
