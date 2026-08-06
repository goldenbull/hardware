cmake_minimum_required(VERSION 3.16)

set(WIFI_SSID "" CACHE STRING "Wi-Fi SSID")
set(WIFI_PASSWORD "" CACHE STRING "Wi-Fi password")
set(NTP_SERVER "ntp.aliyun.com" CACHE STRING "NTP server hostname")
set(TIMEZONE "CST-8" CACHE STRING "POSIX timezone string")

foreach(value WIFI_SSID WIFI_PASSWORD NTP_SERVER TIMEZONE)
    string(REPLACE "\\" "\\\\" ${value}_ESCAPED "${${value}}")
    string(REPLACE "\"" "\\\"" ${value}_ESCAPED "${${value}_ESCAPED}")
    string(REPLACE ";" "\\;" ${value}_ESCAPED "${${value}_ESCAPED}")
endforeach()

add_compile_definitions(
    M5GFX_BOARD=board_M5Stack
    APP_WIFI_SSID="${WIFI_SSID_ESCAPED}"
    APP_WIFI_PASSWORD="${WIFI_PASSWORD_ESCAPED}"
    APP_NTP_SERVER="${NTP_SERVER_ESCAPED}"
    APP_TIMEZONE="${TIMEZONE_ESCAPED}"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(m5stack_clock)
