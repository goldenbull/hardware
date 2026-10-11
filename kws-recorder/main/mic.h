/*
 * 板载麦克风：ES8311 ADC → I2S0，16kHz / 16bit / 单声道。
 *
 * 这个文件以后会原样搬到识别固件里。训练数据和识别时的输入必须走同一条链路——
 * 同样的采样率、同样的 PGA 增益、同样的 I2S 配置，否则模型在板子上的表现会和
 * 在 Mac 上验证的对不上。所以这里的参数一律写死，不提供运行时修改的接口。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define MIC_SAMPLE_RATE      16000
#define MIC_BITS_PER_SAMPLE  16
#define MIC_CHANNELS         1

/* ES8311 模拟 PGA 增益（dB），只有 0/6/12/.../42 这几档，见 menuconfig */
#define MIC_GAIN_DB          CONFIG_KWS_MIC_GAIN_DB

/* 初始化 I2S + ES8311 并开始采集。I2C 必须已经初始化 */
esp_err_t mic_init(void);

/* 阻塞读 samples 个采样点。采集是连续的，调用方要读得够快，否则 DMA 会覆盖旧数据 */
esp_err_t mic_read(int16_t *buf, size_t samples);
