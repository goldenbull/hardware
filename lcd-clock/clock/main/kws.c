#include "kws.h"

#include "esp_log.h"

static const char *TAG = "kws";

/*
 * TODO：接入识别模型。大致步骤：
 *   1. kws_init()：加载模型（放 flash 分区或者编进固件），在 PSRAM 里分配特征和推理缓冲
 *   2. kws_feed()：攒够一帧（比如 30ms 窗 / 10ms 步进）就算一次 MFCC / Fbank，
 *      滑动窗口送进模型，后验概率连续若干帧超过阈值就返回结果
 *   3. 训练时的特征参数必须和这里一致；输入链路已经是同一份 components/mic
 */
esp_err_t kws_init(void)
{
    ESP_LOGW(TAG, "语音识别还没实现，只采集不识别");
    return ESP_OK;
}

kws_result_t kws_feed(const int16_t *pcm, size_t samples)
{
    (void)pcm;
    (void)samples;
    return KWS_NONE;
}
