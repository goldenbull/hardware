#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/async_context.h"
#include "pico/cyw43_arch.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/watchdog.h"

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

void init_pico_status(PicoStatus *status)
{
    status->key_a_prev_is_down = false;
    status->key_a_keydown_handled = true;
    status->key_b_prev_is_down = false;
    status->key_b_keydown_handled = true;
    status->key_bs_prev_is_down = false;
    status->key_bs_keydown_handled = true;

    status->cur_brightness = 30;

    status->ntp_time_fetched = false;
    status->base_abs_time = 0;
    memset(&status->base_tm, 0, sizeof(status->base_tm));
    status->base_microsec = 0;
}

async_at_time_worker_t counter_worker;
async_at_time_worker_t blink_worker;

static void counter_worker_fn(async_context_t *context, async_at_time_worker_t *worker)
{
    async_context_acquire_lock_blocking(context);

    // Paint_Clear(WHITE);
    int val = *(int *)(worker->user_data);
    val++;
    Paint_DrawNum(20, 20, val, &Font20, 3, BLUE, YELLOW);

    absolute_time_t t = get_absolute_time();
    Paint_DrawNum(140, 20, t, &Font16, 0, BLUE, YELLOW);

    if (ntp_state != NULL)
    {
        Paint_DrawString_EN(20, 50, ntp_state->timestr, &Font24, BLUE, WHITE);
    }

    LCD_1IN14_Display(Image);

    // reschedule next time, or restart
    if (val < 500)
    {
        *(int *)worker->user_data = val;
        async_context_add_at_time_worker_in_ms(context, worker, 1000);
    }
    else
    {
        watchdog_reboot(0, 0, 0);
    }

    async_context_release_lock(context);
}

static void blink_worker_fn(async_context_t *context, async_at_time_worker_t *worker)
{
    async_context_acquire_lock_blocking(context);

    bool cur_flag = *(bool *)worker->user_data;
    cur_flag = !cur_flag;
    set_led(cur_flag);

    *(bool *)worker->user_data = cur_flag;
    async_context_add_at_time_worker_in_ms(context, worker, 500);

    async_context_release_lock(context);
}

void run_loop()
{
    int counter = 0;
    bool led_is_on = true;

    // add tasks
    counter_worker.do_work = counter_worker_fn;
    counter_worker.user_data = &counter;
    blink_worker.do_work = blink_worker_fn;
    blink_worker.user_data = &led_is_on;
    async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(), &counter_worker, 0);
    async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(), &blink_worker, 0);
    start_query_ntp();

    while (true)
    {
        cyw43_arch_poll();
        // cyw43_arch_wait_for_work_until(at_the_end_of_time);
    }
}

int main()
{
    // initialize dev and libs
    stdio_init_all();

    PicoStatus status;
    init_pico_status(&status);

    init_lcd(&status);

    lcd_log_info(20, 50, "Connecting WiFi...");
    if (init_wifi_connect())
    {
        lcd_log_error(20, 50, "can not connect to wifi");
        return -1;
    }
    else
    {
        lcd_log_info(20, 50, "wifi connected");
    }

    run_loop();

    // bool led_is_on = true;

    // while (true)
    // {
    //     set_led(led_is_on);
    //     led_is_on = !led_is_on;
    //     sleep_ms(500);
    // }

    return 0;
}
