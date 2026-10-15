/* Wi-Fi STA，只在 HTTP 上传模式下用。断线后自动重连，永不放弃 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t wifi_start(const char *ssid, const char *password);

/* 拿到 IP 才算连上 */
bool wifi_is_connected(void);
