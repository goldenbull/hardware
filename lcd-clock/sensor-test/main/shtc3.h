/*
 * Sensirion SHTC3 温湿度传感器，板载，I2C 地址固定 0x70。
 * 这里是给测试用的版本：四种测量模式都开放出来，并且把原始值交给调用方，方便排查读数异常。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#define SHTC3_ADDR 0x70

typedef enum {
    SHTC3_MODE_NORMAL_T_FIRST,      /* 0x7866 正常模式，先出温度 */
    SHTC3_MODE_NORMAL_RH_FIRST,     /* 0x58E0 正常模式，先出湿度 */
    SHTC3_MODE_LP_T_FIRST,          /* 0x609C 低功耗模式，先出温度 */
    SHTC3_MODE_LP_RH_FIRST,         /* 0x401A 低功耗模式，先出湿度 */
    SHTC3_MODE_COUNT,
} shtc3_mode_t;

typedef struct {
    uint16_t raw_t;
    uint16_t raw_rh;
    float    temperature_c;
    float    humidity_rh;
} shtc3_sample_t;

const char *shtc3_mode_name(shtc3_mode_t mode);

/* 唤醒、软复位、读 ID 并检查型号位。成功后传感器处于睡眠 */
esp_err_t shtc3_probe(uint16_t *id);

/* 唤醒 → 测量 → 睡眠，带 CRC 校验。不用时钟拉伸，轮询结果 */
esp_err_t shtc3_measure(shtc3_mode_t mode, shtc3_sample_t *out);

/* 手册公式：T = -45 + 175 * raw / 2^16 */
float shtc3_raw_to_celsius(uint16_t raw);
