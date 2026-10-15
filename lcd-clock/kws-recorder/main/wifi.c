#include "wifi.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "wifi";

/* 断线重连退避：1s 起步，每次翻倍，封顶 10s。录音时断网要尽快连回来，封顶比 clock 短 */
#define RETRY_MIN_MS 1000
#define RETRY_MAX_MS 10000

static atomic_bool        s_connected;
static uint32_t           s_retry_delay_ms = RETRY_MIN_MS;
/* 重连用定时器，不在事件回调里 vTaskDelay，免得把事件循环卡住 */
static esp_timer_handle_t s_retry_timer;

bool wifi_is_connected(void)
{
    return atomic_load(&s_connected);
}

static void retry_timer_cb(void *arg)
{
    esp_wifi_connect();
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        atomic_store(&s_connected, false);
        ESP_LOGW(TAG, "断开（原因 %d，RSSI %d），%lu ms 后重连",
                 event->reason, event->rssi, (unsigned long)s_retry_delay_ms);
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, (uint64_t)s_retry_delay_ms * 1000);
        s_retry_delay_ms = (s_retry_delay_ms * 2 > RETRY_MAX_MS) ? RETRY_MAX_MS : s_retry_delay_ms * 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        int rssi = 0;
        esp_wifi_sta_get_rssi(&rssi);
        ESP_LOGI(TAG, "获取到 IP " IPSTR "（RSSI %d dBm）", IP2STR(&event->ip_info.ip), rssi);
        s_retry_delay_ms = RETRY_MIN_MS;
        atomic_store(&s_connected, true);
    }
}

esp_err_t wifi_start(const char *ssid, const char *password)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "擦除 NVS 失败");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "NVS 初始化失败");

    const esp_timer_create_args_t retry_args = {
        .callback = retry_timer_cb,
        .name = "wifi_retry",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&retry_args, &s_retry_timer), TAG, "创建重连定时器失败");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init 失败");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "创建事件循环失败");
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init 失败");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL),
                        TAG, "注册 Wi-Fi 事件失败");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL),
                        TAG, "注册 IP 事件失败");

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, password, sizeof(wc.sta.password));
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "设置 STA 模式失败");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "设置 Wi-Fi 配置失败");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start 失败");

    /*
     * 关掉省电：默认的 MIN_MODEM 每个 DTIM 才醒一次收包，HTTP 回包要多等上百毫秒，
     * 每秒一次上传就会越积越多。板子插着 USB 供电，不在乎这点功耗。
     */
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAG, "连接 %s …", ssid);
    return ESP_OK;
}
