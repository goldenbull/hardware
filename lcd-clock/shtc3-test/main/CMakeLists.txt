# i2c_bus / exio 直接用上一级 kws-recorder 工程的源文件，不复制一份
idf_component_register(
    SRCS
        "app_main.c"
        "shtc3.c"
        "../../main/i2c_bus.c"
        "../../main/exio.c"
    INCLUDE_DIRS "." "../../main"
    REQUIRES
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_spi
        esp_driver_tsens
        esp_timer
)
