/* Wi-Fi 连接 + SNTP 校时 */
#pragma once

#include <stdbool.h>
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
