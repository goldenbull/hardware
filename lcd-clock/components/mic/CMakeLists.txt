# I2S0 + ES8311 麦克风采集。录音工程和最终的识别固件共用这一份，保证训练和识别是同一条链路
idf_component_register(
    SRCS "mic.c"
    INCLUDE_DIRS "."
    REQUIRES board
    PRIV_REQUIRES esp_driver_i2s
)
