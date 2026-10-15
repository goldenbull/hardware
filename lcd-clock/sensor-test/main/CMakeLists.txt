idf_component_register(
    SRCS
        "app_main.c"
        "shtc3.c"
    INCLUDE_DIRS "."
    REQUIRES
        board
        esp_driver_tsens
        esp_timer
)
