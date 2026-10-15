/* Wi-Fi 连接 + SNTP 校时 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

typedef enum {
    NET_STATE_CONNECTING,   /* 正在连 Wi-Fi */
    NET_STATE_SYNCING,      /* Wi-Fi 已连上，等 NTP */
    NET_STATE_READY,        /* 时间已同步 */
    NET_STATE_OFFLINE,      /* 连不上，退到慢速重试 */
} net_state_t;

esp_err_t net_time_start(void);

net_state_t net_time_get_state(void);

/* 是否已经至少成功校时一次 */
bool net_time_is_synced(void);

/* 调试面板用的统计 */
typedef struct {
    uint32_t sync_count;        /* 成功校时次数（含每小时的定期校时） */
    time_t   last_sync;         /* 最近一次校时的时刻，0 表示还没校过 */
    uint32_t disconnect_count;  /* Wi-Fi 断开次数 */
    int      last_disconnect_reason;
} net_stats_t;

void net_time_get_stats(net_stats_t *out);
