#include "voice.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mic.h"

static const char *TAG = "voice";

/* 和 kws-recorder 一样每 20ms 一块 */
#define CHUNK_MS        20
#define CHUNK_SAMPLES   (MIC_SAMPLE_RATE * CHUNK_MS / 1000)

/*
 * 比界面、触摸都高：I2S DMA 缓冲有限，读慢了会丢数据；识别结果晚到几十毫秒无所谓。
 * 栈留得宽一点，以后推理可能直接在这个任务里跑。
 */
#define VOICE_TASK_PRIORITY 5
#define VOICE_TASK_STACK    (8 * 1024)

static voice_cb_t s_cb;

static void voice_task(void *arg)
{
    static int16_t buf[CHUNK_SAMPLES];

    while (1) {
        if (mic_read(buf, CHUNK_SAMPLES) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(CHUNK_MS));
            continue;
        }

        const kws_result_t result = kws_feed(buf, CHUNK_SAMPLES);
        if (result != KWS_NONE) {
            ESP_LOGI(TAG, "识别到：%s", result == KWS_SCREEN_ON ? "打开屏幕" : "关闭屏幕");
            if (s_cb) {
                s_cb(result);
            }
        }
    }
}

esp_err_t voice_start(voice_cb_t on_keyword)
{
    s_cb = on_keyword;

    ESP_RETURN_ON_ERROR(mic_init(), TAG, "麦克风初始化失败");
    ESP_RETURN_ON_ERROR(kws_init(), TAG, "识别器初始化失败");

    ESP_RETURN_ON_FALSE(xTaskCreate(voice_task, "voice", VOICE_TASK_STACK, NULL, VOICE_TASK_PRIORITY, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "创建语音任务失败");

    ESP_LOGI(TAG, "语音开关屏已启用，增益 %ddB", MIC_GAIN_DB);
    return ESP_OK;
}
