#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/async_context.h"
#include "pico/cyw43_arch.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/watchdog.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#include "simple_clock.h"
#include "lib_lcd.h"
#include "lib_wifi.h"
#include "lib_bootsel.h"

/*********************************************************************************************

功能设计：
- 启动后连接WiFi，通过NTP获得时间，每小时同步一次NTP
- AB按钮调节亮度
- BOOTSEL按钮重启

*/

PicoStatus pico_status = {0}; // global status

void init_pico_status()
{
    pico_status.key_a_prev_is_down = false;
    pico_status.key_b_prev_is_down = false;
    pico_status.key_ctrl_prev_is_down = false;

    pico_status.cur_brightness = 10;
    pico_status.brightness_levels[0] = 0;
    pico_status.brightness_levels[1] = 10;
    pico_status.brightness_levels[2] = 30;
    pico_status.brightness_levels[3] = 80;

    pico_status.ntp_time_fetched = false;
    memset(pico_status.ntp_err_msg, 0, sizeof(pico_status.ntp_err_msg));
    pico_status.base_abs_time = 0;
    pico_status.base_ntp_ts = 0;
    pico_status.base_microsec = 0;
}

void check_keys()
{
    bool key_a_is_down = DEV_Digital_Read(KEY_A) == 0;
    bool key_b_is_down = DEV_Digital_Read(KEY_B) == 0;
    bool key_ctrl_is_down = DEV_Digital_Read(KEY_ctrl) == 0;

    if (!pico_status.key_a_prev_is_down && key_a_is_down)
    {
        // key a pressed, increase to next level
        for (int i = 0; i < 4; i++)
        {
            if (pico_status.brightness_levels[i] > pico_status.cur_brightness)
            {
                pico_status.cur_brightness = MIN(100, pico_status.brightness_levels[i]);
                break;
            }
        }

        DEV_SET_PWM(pico_status.cur_brightness);
    }

    if (!pico_status.key_b_prev_is_down && key_b_is_down)
    {
        // key b pressed
        for (int i = 3; i >= 0; i--)
        {
            if (pico_status.brightness_levels[i] < pico_status.cur_brightness)
            {
                pico_status.cur_brightness = MAX(0, pico_status.brightness_levels[i]);
                break;
            }
        }

        DEV_SET_PWM(pico_status.cur_brightness);
    }

    if (!pico_status.key_ctrl_prev_is_down && key_ctrl_is_down)
    {
        // bootsel pressed, reboot
        cyw43_arch_deinit();
        watchdog_reboot(0, 0, 0);
    }

    pico_status.key_a_prev_is_down = key_a_is_down;
    pico_status.key_b_prev_is_down = key_b_is_down;
    pico_status.key_ctrl_prev_is_down = key_ctrl_is_down; // actually, not needed
}

int main()
{
    // initialize dev and libs
    stdio_init_all();
    init_pico_status();
    cyw43_arch_init();
    cyw43_arch_enable_sta_mode();
    init_lcd();

    // connect WIFI
    int ret = cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 10000);
    if (ret)
    {
        char msg[1024];
        sprintf(msg, "connect failed: %d", ret);
        lcd_log_error(20, 50, msg);
        return -1; // or exit(), nothing to do
    }

    // 开始同步NTP
    start_query_ntp();
    while (!pico_status.ntp_time_fetched)
    {
        if (pico_status.ntp_err_msg[0] != 0)
        {
            Paint_Clear(BLACK);
            lcd_log_error(10, 50, pico_status.ntp_err_msg);
        }
        sleep_ms(100);
    }

    // 主循环，刷新显示，处理按键
    Paint_Clear(BLACK);
    DEV_SET_PWM(pico_status.cur_brightness);
    char buf[1024];
    while (true)
    {
        absolute_time_t cur_abs_ts = get_absolute_time();
        int64_t diff_us = absolute_time_diff_us(pico_status.base_abs_time, cur_abs_ts);
        time_t cur_ts = pico_status.base_ntp_ts + diff_us / 1000000;
        double frac_sec = diff_us % 1000000 / 1000000.0;
        struct tm now;
        gmtime_r(&cur_ts, &now);

        // show on lcd
        // Paint_Clear(BLACK);

        // date
        sprintf(buf, "%04d-%02d-%02d", now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
        Paint_DrawString_EN(30, 40, buf, &Font24, WHITE, BLACK);

        // time
        sprintf(buf, "%02d:%02d:%02d.%02d", now.tm_hour + 8, now.tm_min, now.tm_sec, (int)(frac_sec * 100)); // timezone hardcoded to +8
        Paint_DrawString_EN(20, 80, buf, &Font24, WHITE, BLACK);

        check_keys();

        // TODO: special image for special keys
        LCD_1IN14_Display(Image);
    }

    return 0;
}
