# TF 卡，SPI 模式，和屏共用 SPI2，必须在 display_init() 之后挂载
idf_component_register(
    SRCS "sdcard.c"
    INCLUDE_DIRS "."
    REQUIRES board
    PRIV_REQUIRES esp_driver_sdspi fatfs sdmmc vfs
)
