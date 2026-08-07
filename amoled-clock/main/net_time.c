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
/* 快速重试用完后的慢速重连间隔 */
#define SLOW_RETRY_MS         30000

static net_state_t s_state  = NET_STATE_CONNECTING;
static bool        s_synced = false;
static int         s_retry  = 0;

static void on_time_synced(struct timeval *tv)
{
    s_synced = true;
    s_state = NET_STATE_READY;

    time_t now = tv->tv_sec;
    struct tm local;
    localtime_r(&now, &local);
    ESP_LOGI(TAG, "NTP 校时成功：%04d-%02d-%02d %02d:%02d:%02d",
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec);
}

/*
 * 慢速重连用定时器而不是在事件回调里 vTaskDelay —— 事件回调跑在
 * 事件循环任务上，堵在那里会把后面所有网络事件一起卡住。
 */
static esp_timer_handle_t s_retry_timer;

static void retry_timer_cb(void *arg)
{
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_synced && s_retry < CONFIG_APP_WIFI_MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "Wi-Fi 断开，第 %d 次重连", s_retry);
            esp_wifi_connect();
            return;
        }
        /* 已经校过时的话掉线不影响走时，转成低频重连即可 */
        s_state = NET_STATE_OFFLINE;
        ESP_LOGW(TAG, "Wi-Fi 不可用，%d 秒后重试", SLOW_RETRY_MS / 1000);
        esp_timer_start_once(s_retry_timer, (uint64_t)SLOW_RETRY_MS * 1000);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "获取到 IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry = 0;
        s_state = s_synced ? NET_STATE_READY : NET_STATE_SYNCING;
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

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            wifi_event_handler, NULL, NULL),
                        TAG, "注册 WIFI_EVENT 失败");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            wifi_event_handler, NULL, NULL),
                        TAG, "注册 IP_EVENT 失败");

    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)wifi_cfg.sta.ssid, CONFIG_APP_WIFI_SSID, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, CONFIG_APP_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password) - 1);

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "set_config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");

    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_APP_NTP_SERVER);
    sntp_cfg.start = true;
    sntp_cfg.server_from_dhcp = false;
    sntp_cfg.sync_cb = on_time_synced;
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&sntp_cfg), TAG, "esp_netif_sntp_init failed");
    esp_sntp_set_sync_interval(SNTP_SYNC_INTERVAL_MS);

    ESP_LOGI(TAG, "连接 %s，NTP 服务器 %s，时区 %s",
             CONFIG_APP_WIFI_SSID, CONFIG_APP_NTP_SERVER, CONFIG_APP_TIMEZONE);
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
