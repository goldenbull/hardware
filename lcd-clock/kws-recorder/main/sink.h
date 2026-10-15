/*
 * 录音落盘的去处：TF 卡（sink_sd.c）或者 HTTP 上传到电脑（sink_http.c），
 * menuconfig → 录音存储 里二选一，编译时只有一个生效。
 *
 * 这些函数都只在 recorder.c 的写盘任务里调用，可以阻塞（写卡偶尔卡几百毫秒，
 * HTTP 超时要几秒），采集那边靠 PSRAM 里的环形缓冲兜住。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

typedef enum {
    SINK_OK = 0,
    SINK_RETRY,     /* 暂时失败（网络断了），数据没收下，过一会儿原样再交一次 */
    SINK_FAIL,      /* 彻底失败（卡满、服务器拒收），本次录音到此为止 */
} sink_result_t;

/* 一次录音结束时记进 sessions.csv 的内容 */
typedef struct {
    const char *file;
    const char *keyword;
    uint32_t    samples;
    uint32_t    dropped_samples;
    uint32_t    clipped_samples;
    uint32_t    wall_ms;
    bool        complete;   /* 数据是否全部落盘 */
} sink_log_t;

/*
 * 空闲时多久调一次 sink_refresh()。TF 卡查剩余空间要扫 FAT 表，很慢，不轮询；
 * HTTP 要定期看服务器还在不在，顶栏状态才是准的。
 */
#if CONFIG_REC_STORAGE_HTTP
#define SINK_POLL_TICKS pdMS_TO_TICKS(3000)
#else
#define SINK_POLL_TICKS portMAX_DELAY
#endif

/* 开机调一次：挂 TF 卡 / 连 Wi-Fi。失败也返回 ESP_OK，开始录音时会再试 */
esp_err_t sink_init(void);

/* 刷新顶栏的存储状态文字，ok 为 false 时界面标黄 */
void sink_refresh(char *text, size_t text_len, bool *ok);

/* 新建一段录音，name 里返回文件名（不含目录）。失败返回 false，*error 是给界面看的原因 */
bool sink_open(const char *prefix, char *name, size_t name_len, const char **error);

/* 追加一块 PCM，offset 是这块数据之前已经成功落盘的字节数 */
sink_result_t sink_write(const void *data, size_t len, uint32_t offset, const char **error);

/* 录音中定期调用：TF 卡回写 WAV 头并 fsync，中途断电也不坏文件；HTTP 什么都不做 */
bool sink_checkpoint(uint32_t data_bytes);

/* 收尾：补好文件头、记一行 sessions.csv */
void sink_close(uint32_t data_bytes, const sink_log_t *log);
