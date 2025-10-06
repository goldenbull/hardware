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

PicoStatus status; // global status

void init_pico_status()
{
    status.key_a_prev_is_down = false;
    status.key_a_keydown_handled = true;
    status.key_b_prev_is_down = false;
    status.key_b_keydown_handled = true;
    status.key_bs_prev_is_down = false;
    status.key_bs_keydown_handled = true;

    status.cur_brightness = 30;

    status.ntp_time_fetched = false;
    status.base_abs_time = 0;
    memset(&status.base_tm, 0, sizeof(status.base_tm));
    status.base_microsec = 0;
}

async_at_time_worker_t counter_worker;
async_at_time_worker_t blink_worker;

static void counter_worker_fn(async_context_t *context, async_at_time_worker_t *worker)
{
    // async_context_acquire_lock_blocking(context);

    Paint_Clear(BLACK);
    int val = *(int *)(worker->user_data);
    val++;
    Paint_DrawNum(20, 20, val, &Font20, 3, WHITE, BLACK);

    absolute_time_t t = get_absolute_time();
    Paint_DrawNum(140, 20, t, &Font16, 0, WHITE, BLACK);

    if (ntp_state != NULL)
    {
        Paint_DrawString_EN(10, 50, ntp_state->timestr, &Font16, WHITE, BLACK);
    }

    LCD_1IN14_Display(Image);

    // reschedule next time, or restart
    // if (val < 500)
    {
        *(int *)worker->user_data = val;
        async_context_add_at_time_worker_in_ms(context, worker, 80);
    }
    // else
    {
        // watchdog_reboot(0, 0, 0);
    }

    // async_context_release_lock(context);
}

static void blink_worker_fn(async_context_t *context, async_at_time_worker_t *worker)
{
    // async_context_acquire_lock_blocking(context);

    bool cur_flag = *(bool *)worker->user_data;
    cur_flag = !cur_flag;
    set_led(cur_flag);

    *(bool *)worker->user_data = cur_flag;
    async_context_add_at_time_worker_in_ms(context, worker, 500);

    // async_context_release_lock(context);
}

void run_loop()
{
    int ret = start_query_ntp();
    // Paint_Clear(BLACK);
    // Paint_DrawString_EN(20, 50, "start_query_ntp", &Font16, BLUE, WHITE);
    // Paint_DrawNum(20, 60, ret, &Font16, 0, WHITE, BLACK);
    // LCD_1IN14_Display(Image);
    // sleep_ms(1000);

    while (!ntp_state->ntp_synchronized)
    {
        sleep_ms(100);
    }

    init_lcd();

    Paint_Clear(BLACK);
    Paint_DrawString_EN(20, 50, "ntp synchronized!", &Font16, BLUE, WHITE);
    LCD_1IN14_Display(Image);
    // sleep_ms(1000);

    int counter = 0;
    bool led_is_on = true;

    // add tasks
    counter_worker.do_work = counter_worker_fn;
    counter_worker.user_data = &counter;
    blink_worker.do_work = blink_worker_fn;
    blink_worker.user_data = &led_is_on;
    async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(), &counter_worker, 0);
    async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(), &blink_worker, 0);

    while (true)
    {
        sleep_ms(100);
        // cyw43_arch_poll();
        // cyw43_arch_wait_for_work_until(at_the_end_of_time);
    }
}

void simple_loop()
{
    int counter = 0;
    while (true)
    {
        counter++;
        absolute_time_t t = get_absolute_time();
        Paint_DrawNum(20, 20, counter, &Font16, 0, WHITE, BLACK);
        Paint_DrawNum(120, 20, t, &Font16, 0, WHITE, BLACK);
        LCD_1IN14_Display(Image);
        sleep_ms(100);
    }
}

int main()
{
    // initialize dev and libs
    stdio_init_all();
    init_pico_status();
    // init_lcd();

    // connect WIFI
    cyw43_arch_init();
    cyw43_arch_enable_sta_mode();
    int ret = cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 10000);

    // Paint_Clear(BLACK);
    // if (ret)
    // {
    //     char msg[1024];
    //     sprintf(msg, "async connect failed: %d", ret);
    //     lcd_log_error(20, 60, msg);
    // }
    // else
    // {
    //     lcd_log_info(20, 60, "connected");
    // }

    run_loop();
    // simple_loop();

    return 0;
}
