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

#include "ImageData.h"
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
    memset(&pico_status.prev_keys, 0, sizeof(pico_status.prev_keys));

    pico_status.cur_brightness = 10;
    pico_status.brightness_levels[0] = 0;
    pico_status.brightness_levels[1] = 10;
    pico_status.brightness_levels[2] = 30;
    pico_status.brightness_levels[3] = 80;
    pico_status.show_heart = false;

    pico_status.ntp_time_fetched = false;
    memset(pico_status.ntp_err_msg, 0, sizeof(pico_status.ntp_err_msg));
    pico_status.base_abs_time = 0;
    pico_status.base_ntp_ts = 0;
    pico_status.base_microsec = 0;
}

void check_keys()
{
    LcdKeyStatus cur_keys = {false};
    cur_keys.a = DEV_Digital_Read(KEY_A) == 0;
    cur_keys.b = DEV_Digital_Read(KEY_B) == 0;
    cur_keys.up = DEV_Digital_Read(KEY_up) == 0;
    cur_keys.down = DEV_Digital_Read(KEY_dowm) == 0;
    cur_keys.left = DEV_Digital_Read(KEY_left) == 0;
    cur_keys.right = DEV_Digital_Read(KEY_right) == 0;
    cur_keys.ctrl = DEV_Digital_Read(KEY_ctrl) == 0;

    if (!pico_status.prev_keys.up && cur_keys.up)
    {
        // key up pressed, increase to next level
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

    if (!pico_status.prev_keys.down && cur_keys.down)
    {
        // key down
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

    if (!(pico_status.prev_keys.ctrl && pico_status.prev_keys.a) && (cur_keys.ctrl && cur_keys.a))
    {
        // 小彩蛋
        pico_status.show_heart = true;
        Paint_Clear(BLACK);
    }
    else if ((pico_status.prev_keys.ctrl && pico_status.prev_keys.a) && !(cur_keys.ctrl && cur_keys.a))
    {
        pico_status.show_heart = false;
        Paint_Clear(BLACK);
    }

    if (cur_keys.ctrl && cur_keys.b)
    {
        // reboot
        cyw43_arch_deinit();
        watchdog_reboot(0, 0, 0);
    }

    pico_status.prev_keys = cur_keys;
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
        check_keys();

        if (pico_status.show_heart)
        {
            // LCD h135*w240
            // image h135*w128
            Paint_DrawImage((const unsigned char *)gImg_my_heart, (240 - 128) / 2, 0, 128, 135);
        }
        else
        {
            absolute_time_t cur_abs_ts = get_absolute_time();
            int64_t diff_us = absolute_time_diff_us(pico_status.base_abs_time, cur_abs_ts);
            time_t cur_ts = pico_status.base_ntp_ts + diff_us / 1000000;
            double frac_sec = diff_us % 1000000 / 1000000.0;
            struct tm now;
            gmtime_r(&cur_ts, &now);

            // date
            sprintf(buf, "%04d-%02d-%02d", now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
            Paint_DrawString_EN(13, 40, buf, &Font24, WHITE, BLACK);

            static const char *daynames[] = {"日", "一", "二", "三", "四", "五", "六"};
            sprintf(buf, "%s", daynames[now.tm_wday]);
            Paint_DrawString_CN(188, 36, buf, &Font21CN, WHITE, BLACK);

            // time
            sprintf(buf, "%02d:%02d:%02d.%02d", now.tm_hour + 8, now.tm_min, now.tm_sec, (int)(frac_sec * 100)); // timezone hardcoded to +8
            Paint_DrawString_EN(20, 80, buf, &Font24, WHITE, BLACK);
        }

        // show image in memory
        LCD_1IN14_Display(Image);
    }

    return 0;
}
