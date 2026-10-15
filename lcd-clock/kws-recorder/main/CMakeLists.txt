idf_component_register(
    SRCS
        "app_main.c"
        "recorder.c"
        "ui.c"
        "fonts/font_sc_16.c"
        "fonts/font_sc_24.c"
        "fonts/font_sc_40.c"
    INCLUDE_DIRS "." "fonts"
    REQUIRES
        board
        display
        sdcard
        mic
        esp_ringbuf
        esp_timer
)
