/*
 * 训练数据录音。
 *
 * 麦克风一开机就连续采集（界面上的电平条随时能看），按下"开始录音"后才把采样
 * 落盘。每按一次开始就新建一个文件，编号递增，永远不覆盖之前的录音：
 *
 *   open_001.wav    "打开屏幕"
 *   close_001.wav   "关闭屏幕"
 *   sessions.csv    每次录音一行：文件名、时长、丢帧、削顶等
 *
 * 存到哪里由 menuconfig → 录音存储 决定：TF 卡的 /sdcard/kws/，或者 HTTP 上传到
 * 电脑（tools/upload_server.py），见 sink.h。
 *
 * 任务划分：
 *   rec_cap —— 优先级最高，每 20ms 从 I2S 读一块，算电平，录音中就塞进环形缓冲
 *   rec_wr  —— 从环形缓冲取数据落盘。写卡偶尔卡几百毫秒、Wi-Fi 偶尔断几秒，
 *              环形缓冲放在 PSRAM 里有 2 分钟多，采集端不会因此丢数据
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    KEYWORD_OPEN = 0,   /* 打开屏幕 */
    KEYWORD_CLOSE,      /* 关闭屏幕 */
    KEYWORD_COUNT,
} keyword_t;

typedef enum {
    REC_IDLE = 0,
    REC_STARTING,       /* 正在挂卡 / 向服务器建文件 */
    REC_RECORDING,
    REC_STOPPING,       /* 把环形缓冲里剩下的写完 / 传完，补文件头 */
} rec_state_t;

typedef struct {
    rec_state_t state;
    keyword_t   keyword;
    char        file[24];         /* 当前（或上一次）的文件名，不含目录 */
    uint32_t    captured_samples; /* 本次已录下的采样点数（录音时长按这个算） */
    uint32_t    samples;          /* 其中已经落盘的，比 captured 少的部分还在缓冲里 */
    uint32_t    dropped_samples;  /* 环形缓冲满了丢掉的采样点数，正常应该一直是 0 */
    uint32_t    clipped_samples;  /* 削顶的采样点数，持续增长说明增益太大或离得太近 */
    uint16_t    peak;             /* 自上次查询以来的峰值绝对值，0~32767，做电平条用 */
    const char *error;            /* 上一次失败的原因，或者正在重试的原因，没有就是 NULL */
    bool        storage_ok;       /* 存储可用（卡挂上了 / 服务器连得上） */
    char        storage[32];      /* 顶栏的存储状态文字，比如"TF 剩余 28.3 G" */
} rec_status_t;

/* mic_init() 之后调用，挂卡 / 连 Wi-Fi，起采集和写盘两个任务 */
esp_err_t recorder_init(void);

/* 下面两个只是发命令，立刻返回；实际状态看 recorder_get_status() */
void recorder_start(keyword_t keyword);
void recorder_stop(void);

/* 读一份状态快照，同时把 peak 清零 */
void recorder_get_status(rec_status_t *out);

const char *recorder_keyword_text(keyword_t keyword);
