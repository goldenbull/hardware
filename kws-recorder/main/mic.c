#include "mic.h"

#include "board_pins.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "i2c_bus.h"

static const char *TAG = "mic";

/*
 * I2S DMA 缓冲：6 块 x 20ms = 120ms。读的那头（recorder 的采集任务）优先级最高、
 * 每 20ms 读一次，不碰 TF 卡，120ms 余量足够应付屏刷新之类的抢占。
 */
#define I2S_DMA_DESC_NUM   6
#define I2S_DMA_FRAME_NUM  (MIC_SAMPLE_RATE / 50)

static esp_codec_dev_handle_t s_codec;

esp_err_t mic_init(void)
{
    /*
     * 和官方 BSP 一样开全双工：ES8311 的 ADC 和 DAC 共用 BCLK/WS，以后识别固件要放
     * 提示音时不用改这里的初始化。本工程不往 TX 写数据，auto_clear 让它一直发 0。
     */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = I2S_DMA_FRAME_NUM;
    chan_cfg.auto_clear = true;

    i2s_chan_handle_t tx = NULL, rx = NULL;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s_new_channel failed");

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = PIN_NUM_I2S_MCLK,
            .bclk = PIN_NUM_I2S_BCLK,
            .ws = PIN_NUM_I2S_WS,
            .dout = PIN_NUM_I2S_DOUT,
            .din = PIN_NUM_I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "init tx failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "init rx failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "enable tx failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "enable rx failed");

    audio_codec_i2s_cfg_t i2s_if_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx,
        .tx_handle = tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_if_cfg);
    ESP_RETURN_ON_FALSE(data_if, ESP_FAIL, TAG, "audio_codec_new_i2s_data failed");

    audio_codec_i2c_cfg_t i2c_if_cfg = {
        .port = I2C_PORT_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus_get_handle(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_if_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "audio_codec_new_i2c_ctrl failed");

    /*
     * use_mclk=false：板上没接 MCLK，ES8311 用 BCLK 倍频出内部时钟。
     * 功放不交给 codec 驱动管（它在 IO 扩展上，不是 GPIO），录音期间由 exio 保持关闭。
     */
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = -1,
        .master_mode = false,
        .use_mclk = false,
        .digital_mic = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "ES8311 无响应");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec, ESP_FAIL, TAG, "esp_codec_dev_new failed");

    /* ES8311 的 ADC 数据在左声道（右声道是 DAC 回采参考），只取左边 */
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = MIC_BITS_PER_SAMPLE,
        .channel = MIC_CHANNELS,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .sample_rate = MIC_SAMPLE_RATE,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_codec, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "打开 ES8311 失败");

    /* es8311 驱动要求 open 之后才能设增益 */
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_gain(s_codec, MIC_GAIN_DB) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "设置麦克风增益失败");

    ESP_LOGI(TAG, "麦克风就绪：%dHz / %dbit / %d 声道，PGA %ddB",
             MIC_SAMPLE_RATE, MIC_BITS_PER_SAMPLE, MIC_CHANNELS, MIC_GAIN_DB);
    return ESP_OK;
}

esp_err_t mic_read(int16_t *buf, size_t samples)
{
    const int ret = esp_codec_dev_read(s_codec, buf, samples * sizeof(int16_t));
    return ret == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}
