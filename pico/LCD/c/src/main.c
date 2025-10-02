#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/watchdog.h"
#include "pico/async_context.h"

#include "lib_lcd.h"
#include "lib_wifi.h"
#include "lib_bootsel.h"

async_at_time_worker_t counter_worker;
async_at_time_worker_t blink_worker;

static void counter_worker_fn(async_context_t *context, async_at_time_worker_t *worker)
{
    async_context_acquire_lock_blocking(context);

    Paint_Clear(WHITE);
    int val = *(int *)(worker->user_data);
    val++;
    Paint_DrawNum(20, 20, val, &Font20, 3, BLUE, YELLOW);
    // Paint_DrawString_EN(20, 50, "16:34:56", &Font24, RED, WHITE);
    LCD_1IN14_Display(Image);

    // reschedule next time, or restart
    if (val < 5)
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
    init_wifi();
    init_lcd();

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
