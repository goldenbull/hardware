# 外接 Sensirion SHT40/41/45，代码来自 amoled-clock
idf_component_register(
    SRCS "sht4x.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES board esp_driver_gpio
)
