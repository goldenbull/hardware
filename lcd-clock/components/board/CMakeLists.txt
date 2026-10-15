# 板级基础：引脚表、I2C0 总线、CH32V003 IO 扩展。所有工程都要用
idf_component_register(
    SRCS "i2c_bus.c" "exio.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_spi
)
