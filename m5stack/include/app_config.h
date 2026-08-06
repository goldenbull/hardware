#pragma once

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef APP_WIFI_SSID
#define APP_WIFI_SSID ""
#endif

#ifndef APP_WIFI_PASSWORD
#define APP_WIFI_PASSWORD ""
#endif

#ifndef APP_NTP_SERVER
#define APP_NTP_SERVER "ntp.aliyun.com"
#endif

#ifndef APP_TIMEZONE
#define APP_TIMEZONE "CST-8"
#endif
