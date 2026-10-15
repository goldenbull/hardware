# ST7796 屏 + FT6336 触摸，接成 LVGL 8 的显示和输入设备
idf_component_register(
    SRCS "display.c" "touch.c"
    INCLUDE_DIRS "."
    REQUIRES board
    PRIV_REQUIRES esp_lcd esp_timer
)
