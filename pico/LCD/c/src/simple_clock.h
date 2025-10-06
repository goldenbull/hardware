#ifndef MY_SIMPLE_CLOCK
#define MY_SIMPLE_CLOCK

#include <time.h>

typedef struct 
{
    // keys
    bool key_a_prev_is_down;
    bool key_a_keydown_handled;
    bool key_b_prev_is_down;
    bool key_b_keydown_handled;
    bool key_bs_prev_is_down;
    bool key_bs_keydown_handled;

    // LCD
    int cur_brightness;
    uint16_t *Image;

    // timestamp
    bool ntp_time_fetched;
    absolute_time_t base_abs_time;
    struct tm base_tm;
    int base_microsec;
} PicoStatus;

#endif