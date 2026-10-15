#include "net_time.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "net_time";

/* 每小时重新对一次时，抵消晶振漂移 */
#define SNTP_SYNC_INTERVAL_MS (60 * 60 * 1000)

/* 断线重连退避：1s 起步，每次翻倍，封顶 30s，永不放弃。拿到 IP 后归位 */
#define RETRY_MIN_MS          1000
#define RETRY_MAX_MS          30000

/*
 * 关联上 AP 之后多久还没拿到 IP 就算 DHCP 卡住。信号弱的时候 DHCP 包丢了，
 * lwIP 的 DHCP 客户端自己会退避重发，可能拖一分钟以上；主动断开重连，
 * 重新关联 + 重新 DISCOVER 往往更快。
 */
#define DHCP_TIMEOUT_MS       20000

/*
 * 拿到 IP 之后多久还没校上时就重连一次 Wi-Fi（换一轮 DHCP / DNS）。
 * lwIP 每台服务器等 15 秒没回应就换下一台，4 台转一圈 60 秒，这里给两圈。
 */
#define NTP_TIMEOUT_MS        120000

/* 低于这个 RSSI 在日志里提示一下，方便判断是不是信号问题 */
#define RSSI_WEAK_DBM         (-75)

/*
 * Kconfig 里配的是主服务器，后面几个是备用。lwIP 在一台超时后会自动换下一台，
 * 所以只要有一台能通就能校上。实际用几台受 CONFIG_LWIP_SNTP_MAX_SERVERS 限制。
 */
static const char *const s_ntp_servers[] = {
    CONFIG_APP_NTP_SERVER,
    "ntp.tencent.com",
    "ntp.ntsc.ac.cn",
    "cn.pool.ntp.org",
};
#define NTP_SERVER_COUNT (sizeof(s_ntp_servers) / sizeof(s_ntp_servers[0]))

static net_state_t s_state  = NET_STATE_CONNECTING;
static bool        s_synced = false;
static bool        s_has_ip = false;
static uint32_t    s_retry_delay_ms = RETRY_MIN_MS;
static net_stats_t s_stats;

/*
 * 重连和超时都用定时器而不是在事件回调里 vTaskDelay —— 事件回调跑在
 * 事件循环任务上，堵在那里会把后面所有网络事件一起卡住。
 */
static esp_timer_handle_t s_retry_timer;     /* 到点 esp_wifi_connect() */
static esp_timer_handle_t s_watchdog_timer;  /* DHCP / NTP 超时 */

static void on_time_synced(struct timeval *tv)
{
    s_synced = true;
    s_state = NET_STATE_READY;
    s_stats.sync_count++;
    s_stats.last_sync = tv->tv_sec;
    esp_timer_stop(s_watchdog_timer);

    time_t now = tv->tv_sec;
    struct tm local;
    localtime_r(&now, &local);
    ESP_LOGI(TAG, "NTP 校时成功：%04d-%02d-%02d %02d:%02d:%02d",
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec);
}

static void retry_timer_cb(void *arg)
{
    esp_wifi_connect();
}

/* 定时器在跑的时候 esp_timer_start_once 会失败，所以先停再启 */
static void restart_timer(esp_timer_handle_t timer, uint32_t ms)
{
    esp_timer_stop(timer);
    esp_timer_start_once(timer, (uint64_t)ms * 1000);
}

/*
 * DHCP 或 NTP 超时：主动断开，断开事件会走正常的退避重连流程。
 * 跑在 esp_timer 任务里，调 esp_wifi_disconnect() 没问题。
 */
static void watchdog_timer_cb(void *arg)
{
    if (s_has_ip) {
        ESP_LOGW(TAG, "拿到 IP %d 秒仍未校时，重连 Wi-Fi", NTP_TIMEOUT_MS / 1000);
    } else {
        ESP_LOGW(TAG, "关联 AP %d 秒仍未拿到 IP，重连 Wi-Fi", DHCP_TIMEOUT_MS / 1000);
    }
    esp_wifi_disconnect();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        int rssi = 0;
        esp_wifi_sta_get_rssi(&rssi);
        if (rssi < RSSI_WEAK_DBM) {
            ESP_LOGW(TAG, "已关联 AP，但信号弱（RSSI %d dBm），DHCP / NTP 可能丢包", rssi);
        } else {
            ESP_LOGI(TAG, "已关联 AP（RSSI %d dBm），等待 DHCP", rssi);
        }
        restart_timer(s_watchdog_timer, DHCP_TIMEOUT_MS);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        s_has_ip = false;
        esp_timer_stop(s_watchdog_timer);
        s_stats.disconnect_count++;
        s_stats.last_disconnect_reason = event->reason;

        /* 已经校过时的话掉线不影响走时，界面直接显示离线；没校过时就一直显示连接中直到退避封顶 */
        s_state = (s_synced || s_retry_delay_ms >= RETRY_MAX_MS) ? NET_STATE_OFFLINE : NET_STATE_CONNECTING;
        ESP_LOGW(TAG, "Wi-Fi 断开（原因 %d，RSSI %d），%lu ms 后重连",
                 event->reason, event->rssi, (unsigned long)s_retry_delay_ms);
        restart_timer(s_retry_timer, s_retry_delay_ms);
        s_retry_delay_ms = (s_retry_delay_ms * 2 > RETRY_MAX_MS) ? RETRY_MAX_MS : s_retry_delay_ms * 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "获取到 IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_has_ip = true;
        s_retry_delay_ms = RETRY_MIN_MS;

        /*
         * 每次拿到 IP 都重启一次 SNTP，立刻发请求。不在开机时就启动：网络没通
         * 的时候 SNTP 发出去的请求全部失败，还要白等一轮超时才会再试。
         */
        esp_netif_sntp_start();

        if (s_synced) {
            s_state = NET_STATE_READY;
            esp_timer_stop(s_watchdog_timer);
        } else {
            s_state = NET_STATE_SYNCING;
            restart_timer(s_watchdog_timer, NTP_TIMEOUT_MS);
        }
    }
}

esp_err_t net_time_start(void)
{
    /* 时区要在任何 localtime_r 之前设好 */
    setenv("TZ", CONFIG_APP_TIMEZONE, 1);
    tzset();

    const esp_timer_create_args_t retry_args = {
        .callback = retry_timer_cb,
        .name = "wifi_retry",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&retry_args, &s_retry_timer), TAG, "创建重连定时器失败");

    const esp_timer_create_args_t watchdog_args = {
        .callback = watchdog_timer_cb,
        .name = "net_watchdog",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&watchdog_args, &s_watchdog_timer), TAG, "创建超时定时器失败");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");
    esp_netif_create_default_wifi_sta();

    /* SNTP 只做配置不启动，等 GOT_IP 再启动；必须在 esp_wifi_start 之前，免得 IP 先到 */
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_APP_NTP_SERVER);
    const size_t server_count = NTP_SERVER_COUNT < CONFIG_LWIP_SNTP_MAX_SERVERS
                                    ? NTP_SERVER_COUNT : CONFIG_LWIP_SNTP_MAX_SERVERS;
    if (server_count < NTP_SERVER_COUNT) {
        ESP_LOGW(TAG, "CONFIG_LWIP_SNTP_MAX_SERVERS=%d，只用前 %d 台 NTP 服务器",
                 CONFIG_LWIP_SNTP_MAX_SERVERS, (int)server_count);
    }
    sntp_cfg.num_of_servers = server_count;
    for (size_t i = 0; i < server_count; i++) {
        sntp_cfg.servers[i] = s_ntp_servers[i];
    }
    sntp_cfg.start = false;
    sntp_cfg.server_from_dhcp = false;
    sntp_cfg.sync_cb = on_time_synced;
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&sntp_cfg), TAG, "esp_netif_sntp_init failed");
    esp_sntp_set_sync_interval(SNTP_SYNC_INTERVAL_MS);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            wifi_event_handler, NULL, NULL),
                        TAG, "注册 WIFI_EVENT 失败");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            wifi_event_handler, NULL, NULL),
                        TAG, "注册 IP_EVENT 失败");

    /*
     * 全信道扫描 + 按信号排序：家里有多个同名 AP（Mesh / 中继）时连最强的那个，
     * 而不是扫到的第一个。代价是每次连接多花一两百毫秒扫描。
     */
    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .failure_retry_cnt = 3,
        },
    };
    strncpy((char *)wifi_cfg.sta.ssid, CONFIG_APP_WIFI_SSID, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, CONFIG_APP_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password) - 1);

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "set_config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");

    /*
     * 插电的时钟不需要省电。默认的 modem sleep 会让射频按 DTIM 间隔休眠，
     * 信号弱的时候更容易错过 DHCP / NTP 的回包。
     */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "关闭 Wi-Fi 省电失败");

    ESP_LOGI(TAG, "连接 %s，NTP 服务器 %s 等 %d 台，时区 %s",
             CONFIG_APP_WIFI_SSID, CONFIG_APP_NTP_SERVER, (int)server_count, CONFIG_APP_TIMEZONE);
    return ESP_OK;
}

net_state_t net_time_get_state(void)
{
    return s_state;
}

bool net_time_is_synced(void)
{
    return s_synced;
}

void net_time_get_stats(net_stats_t *out)
{
    *out = s_stats;
}
