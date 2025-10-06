#ifndef MY_SIMPLE_CLOCK
#define MY_SIMPLE_CLOCK

#include <time.h>

typedef struct 
{
    // keys
    bool key_a_prev_is_down;
    bool key_b_prev_is_down;
    bool key_ctrl_prev_is_down;

    // LCD
    int cur_brightness;
    int brightness_levels[4]; // predefined levels
    uint16_t *Image;

    // timestamp
    bool ntp_time_fetched;
    char ntp_err_msg[128];
    absolute_time_t base_abs_time;
    time_t base_ntp_ts;
    int base_microsec;
} PicoStatus;

extern PicoStatus pico_status; // global status

#endif