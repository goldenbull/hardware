/*
 * 录音边录边 HTTP 上传到电脑上的 tools/upload_server.py，不需要 TF 卡。
 *
 * 每块 PCM 带着偏移量 POST 上去，服务器按偏移量落盘，所以重发是安全的：
 * 回包丢了、不知道上一块到底收没收到，就原样再发一遍，服务器会截到偏移量再写。
 * 网络断了返回 SINK_RETRY，数据留在 recorder 的缓冲里，连回来接着传。
 */
#include "sink.h"

#if CONFIG_REC_STORAGE_HTTP

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "mic.h"
#include "wifi.h"

static const char *TAG = "sink_http";

/* 局域网里一块 16KB 正常几十毫秒就传完，5 秒还没回来就当网络断了 */
#define HTTP_TIMEOUT_MS   5000
/* 结束时补记 sessions.csv 的重试次数 */
#define FINISH_RETRIES    3

static char s_base_url[128];
static char s_file[24];             /* 当前录音的文件名 */
static esp_http_client_handle_t s_client;
static char s_resp[64];             /* 服务器回的正文都很短：文件名或者一个数字 */
static int  s_resp_len;

static esp_err_t on_http_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        const int room = (int)sizeof(s_resp) - 1 - s_resp_len;
        const int n = e->data_len < room ? e->data_len : room;
        if (n > 0) {
            memcpy(s_resp + s_resp_len, e->data, n);
            s_resp_len += n;
        }
    }
    return ESP_OK;
}

/*
 * 发一个请求，返回 HTTP 状态码；没连上 Wi-Fi 或者网络出错返回 -1。
 * 连接在请求之间保持（服务器是 HTTP/1.1），出错就扔掉，下次重新建。
 */
static int request(esp_http_client_method_t method, const char *path, const void *body, size_t len)
{
    s_resp_len = 0;
    s_resp[0] = '\0';
    if (!wifi_is_connected()) {
        return -1;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s%s", s_base_url, path);

    if (!s_client) {
        const esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = HTTP_TIMEOUT_MS,
            .event_handler = on_http_event,
            .disable_auto_redirect = true,
        };
        s_client = esp_http_client_init(&cfg);
        if (!s_client) {
            ESP_LOGE(TAG, "esp_http_client_init 失败");
            return -1;
        }
    } else {
        esp_http_client_set_url(s_client, url);
    }

    esp_http_client_set_method(s_client, method);
    esp_http_client_set_header(s_client, "Content-Type", "application/octet-stream");
    esp_http_client_set_post_field(s_client, body, (int)len);

    const esp_err_t err = esp_http_client_perform(s_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s 失败：%s", path, esp_err_to_name(err));
        esp_http_client_cleanup(s_client);
        s_client = NULL;
        return -1;
    }
    s_resp[s_resp_len] = '\0';
    return esp_http_client_get_status_code(s_client);
}

esp_err_t sink_init(void)
{
    strlcpy(s_base_url, CONFIG_REC_UPLOAD_URL, sizeof(s_base_url));
    size_t n = strlen(s_base_url);
    while (n > 0 && s_base_url[n - 1] == '/') {
        s_base_url[--n] = '\0';   /* 填的时候带了结尾的 / 也没关系 */
    }
    ESP_LOGI(TAG, "上传到 %s", s_base_url);
    return wifi_start(CONFIG_REC_WIFI_SSID, CONFIG_REC_WIFI_PASSWORD);
}

void sink_refresh(char *text, size_t text_len, bool *ok)
{
    *ok = false;
    if (!wifi_is_connected()) {
        snprintf(text, text_len, "Wi-Fi 连接中");
        return;
    }
    if (request(HTTP_METHOD_GET, "/status", NULL, 0) != 200) {
        snprintf(text, text_len, "服务器连不上");
        return;
    }
    const uint32_t free_mb = (uint32_t)strtoul(s_resp, NULL, 10);
    snprintf(text, text_len, "电脑剩余 %lu.%lu G",
             (unsigned long)(free_mb / 1024), (unsigned long)(free_mb % 1024 * 10 / 1024));
    *ok = true;
}

bool sink_open(const char *prefix, char *name, size_t name_len, const char **error)
{
    s_file[0] = '\0';
    if (!wifi_is_connected()) {
        *error = "Wi-Fi 没连上";
        return false;
    }

    char path[96];
    snprintf(path, sizeof(path), "/start?keyword=%s&rate=%d&channels=%d&bits=%d",
             prefix, MIC_SAMPLE_RATE, MIC_CHANNELS, MIC_BITS_PER_SAMPLE);
    const int code = request(HTTP_METHOD_POST, path, NULL, 0);
    if (code != 200 || s_resp_len == 0 || s_resp_len >= (int)sizeof(s_file)) {
        ESP_LOGE(TAG, "新建录音失败：HTTP %d %s", code, s_resp);
        *error = code < 0 ? "上传服务器连不上" : "服务器建文件失败";
        return false;
    }

    strlcpy(s_file, s_resp, sizeof(s_file));
    strlcpy(name, s_file, name_len);
    ESP_LOGI(TAG, "服务器上新建 %s", s_file);
    return true;
}

sink_result_t sink_write(const void *data, size_t len, uint32_t offset, const char **error)
{
    char path[80];
    snprintf(path, sizeof(path), "/append?file=%s&offset=%lu", s_file, (unsigned long)offset);

    const int code = request(HTTP_METHOD_POST, path, data, len);
    if (code < 0) {
        *error = wifi_is_connected() ? "上传超时，重试中" : "Wi-Fi 断了，等待重连";
        return SINK_RETRY;
    }
    if (code == 200 && strtoul(s_resp, NULL, 10) == offset + len) {
        return SINK_OK;
    }

    ESP_LOGE(TAG, "服务器拒收 %s：HTTP %d %s", path, code, s_resp);
    *error = code == 409 ? "服务器上数据不连续，录音已停止"
           : code >= 500 ? "服务器写文件失败（磁盘满了？）"
           : "服务器拒收，录音已停止";
    return SINK_FAIL;
}

bool sink_checkpoint(uint32_t data_bytes)
{
    return true;   /* 服务器每次追加都回写了 WAV 头 */
}

void sink_close(uint32_t data_bytes, const sink_log_t *log)
{
    if (!s_file[0]) {
        return;
    }

    char path[224];
    snprintf(path, sizeof(path),
             "/finish?file=%s&keyword=%s&duration_s=%.2f&samples=%lu&dropped_samples=%lu"
             "&clipped_samples=%lu&gain_db=%d&sample_rate=%d&wall_ms=%lu&complete=%d",
             log->file, log->keyword, (double)log->samples / MIC_SAMPLE_RATE,
             (unsigned long)log->samples, (unsigned long)log->dropped_samples,
             (unsigned long)log->clipped_samples, MIC_GAIN_DB, MIC_SAMPLE_RATE,
             (unsigned long)log->wall_ms, log->complete ? 1 : 0);

    for (int i = 0; i < FINISH_RETRIES; i++) {
        if (request(HTTP_METHOD_POST, path, NULL, 0) == 200) {
            s_file[0] = '\0';
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    /* 音频已经全在服务器上了，只是 sessions.csv 少一行，不算录音失败 */
    ESP_LOGW(TAG, "%s 的 sessions.csv 记录没传上去", s_file);
    s_file[0] = '\0';
}

#endif /* CONFIG_REC_STORAGE_HTTP */
