#ifndef MY_SIMPLE_CLOCK
#define MY_SIMPLE_CLOCK

#include <time.h>

typedef struct
{
    // true if pressed
    bool up;
    bool down;
    bool left;
    bool right;
    bool ctrl;
    bool a;
    bool b;
} LcdKeyStatus;

typedef struct
{
    // keys
    LcdKeyStatus prev_keys;

    // LCD
    int cur_brightness;
    int brightness_levels[4]; // predefined levels
    bool show_heart;

    // timestamp
    bool ntp_time_fetched;
    char ntp_err_msg[128];
    absolute_time_t base_abs_time;
    time_t base_ntp_ts;
    int base_microsec;
} PicoStatus;

extern PicoStatus pico_status; // global status

#endif