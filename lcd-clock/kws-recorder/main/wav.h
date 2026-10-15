/* 44 字节的标准 PCM WAV 文件头，格式取 mic.h 里的采样参数 */
#pragma once

#include <stdint.h>
#include "mic.h"

typedef struct __attribute__((packed)) {
    char     riff[4];
    uint32_t riff_size;
    char     wave[4];
    char     fmt[4];
    uint32_t fmt_size;
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char     data[4];
    uint32_t data_size;
} wav_header_t;

static inline void wav_fill_header(wav_header_t *h, uint32_t data_bytes)
{
    const uint32_t block_align = MIC_CHANNELS * MIC_BITS_PER_SAMPLE / 8;
    *h = (wav_header_t){
        .riff = {'R', 'I', 'F', 'F'},
        .riff_size = 36 + data_bytes,
        .wave = {'W', 'A', 'V', 'E'},
        .fmt = {'f', 'm', 't', ' '},
        .fmt_size = 16,
        .format = 1,    /* PCM */
        .channels = MIC_CHANNELS,
        .sample_rate = MIC_SAMPLE_RATE,
        .byte_rate = MIC_SAMPLE_RATE * block_align,
        .block_align = block_align,
        .bits_per_sample = MIC_BITS_PER_SAMPLE,
        .data = {'d', 'a', 't', 'a'},
        .data_size = data_bytes,
    };
}
