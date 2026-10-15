/*
 * 训练数据录音。
 *
 * 麦克风一开机就连续采集（界面上的电平条随时能看），按下"开始录音"后才把采样
 * 写进 TF 卡。每按一次开始就新建一个文件，编号递增，永远不覆盖之前的录音：
 *
 *   /sdcard/kws/open_001.wav    "打开屏幕"
 *   /sdcard/kws/close_001.wav   "关闭屏幕"
 *   /sdcard/kws/sessions.csv    每次录音一行：文件名、时长、丢帧、削顶等
 *
 * 任务划分：
 *   rec_cap —— 优先级最高，每 20ms 从 I2S 读一块，算电平，录音中就塞进环形缓冲
 *   rec_wr  —— 从环形缓冲取数据写卡。FAT 写卡偶尔会卡几百毫秒，环形缓冲放在
 *              PSRAM 里有 16 秒，采集端不会因此丢数据
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
    REC_STARTING,       /* 正在挂卡 / 建文件 */
    REC_RECORDING,
    REC_STOPPING,       /* 把环形缓冲里剩下的写完、补文件头 */
} rec_state_t;

typedef struct {
    rec_state_t state;
    keyword_t   keyword;
    char        file[24];         /* 当前（或上一次）的文件名，不含目录 */
    uint32_t    samples;          /* 本次已写入的采样点数 */
    uint32_t    dropped_samples;  /* 环形缓冲满了丢掉的采样点数，正常应该一直是 0 */
    uint32_t    clipped_samples;  /* 削顶的采样点数，持续增长说明增益太大或离得太近 */
    uint16_t    peak;             /* 自上次查询以来的峰值绝对值，0~32767，做电平条用 */
    const char *error;            /* 上一次失败的原因，没有就是 NULL */
    bool        sd_mounted;
    uint32_t    sd_free_mb;       /* 只在开机和每次录音前后刷新 */
} rec_status_t;

/* mic_init() 之后调用，起采集和写卡两个任务 */
esp_err_t recorder_init(void);

/* 下面两个只是发命令，立刻返回；实际状态看 recorder_get_status() */
void recorder_start(keyword_t keyword);
void recorder_stop(void);

/* 读一份状态快照，同时把 peak 清零 */
void recorder_get_status(rec_status_t *out);

const char *recorder_keyword_text(keyword_t keyword);
