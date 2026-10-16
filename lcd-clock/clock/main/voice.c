#include "voice.h"

#include <math.h>
#include <stdatomic.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kws.h"
#include "mic.h"
#include "sdkconfig.h"

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

/* 音量显示 / 平滑的下限，全零输入时 log 不至于算出 -inf */
#define LEVEL_FLOOR_DB      (-96.0f)

#if CONFIG_APP_VOICE_MODE_LEVEL
/* 没勾的 bool 在 sdkconfig.h 里没有定义，补成 0，下面就能直接写在 if / ?: 里 */
#ifndef CONFIG_APP_SOUND_TRIGGER_ENABLE
#define CONFIG_APP_SOUND_TRIGGER_ENABLE 0
#endif

/*
 * 触发之后音量要先回落到阈值以下这么多，才允许下一次触发。不然一阵持续的响声
 * （比如一直在说话）会每过一个冷却期就切一次屏。
 */
#define REARM_HYSTERESIS_DB 6.0f
/* 刚开机 codec 上电、PGA 建立时会有一下"噗"声，这段时间不触发 */
#define WARMUP_MS           1000
#endif

static voice_cb_t  s_cb;
static bool        s_running;
static atomic_uint s_peak;
static atomic_int  s_level_max_db = (int)LEVEL_FLOOR_DB;
static atomic_uint s_triggers;

/* 记下这块的峰值，给调试面板看 */
static void track_peak(const int16_t *buf)
{
    uint32_t peak = 0;
    for (int i = 0; i < CHUNK_SAMPLES; i++) {
        const int32_t v = buf[i];
        const uint32_t a = (uint32_t)(v < 0 ? -v : v);
        if (a > peak) {
            peak = a;
        }
    }
    unsigned int old = atomic_load(&s_peak);
    while (peak > old && !atomic_compare_exchange_weak(&s_peak, &old, peak)) {
    }
}

#if CONFIG_APP_VOICE_MODE_LEVEL

/*
 * 音量触发：每块算一次均方功率，做一阶指数平滑（时间常数 CONFIG_APP_SOUND_SMOOTH_MS），
 * 平滑后的音量从阈值下方升过阈值时算一次触发。
 *
 * 平滑在功率上做而不是在 dB 上做：dB 上平均会让一声很响的短促声音被前后的安静"拉平"，
 * 功率平均才和人耳对响度的感觉接近。冷却（两次开关屏的最小间隔）不在这里，
 * 在 app_main 里统一算，单击开关屏也要计进去。
 */
static bool level_feed(const int16_t *buf)
{
    static float s_power = 0.0f;       /* 平滑后的均方功率，满幅正弦约 0.5 */
    static bool  s_armed = false;      /* 开机时等音量先低于重新布防线再开始 */
    static int   s_warmup = WARMUP_MS / CHUNK_MS;
    /* 一阶 IIR 的系数：每块往新值靠近这么多 */
    static float s_alpha = 0.0f;
    if (s_alpha == 0.0f) {
        s_alpha = 1.0f - expf(-(float)CHUNK_MS / (float)CONFIG_APP_SOUND_SMOOTH_MS);
    }

    float sum = 0.0f;
    for (int i = 0; i < CHUNK_SAMPLES; i++) {
        const float v = buf[i] / 32768.0f;
        sum += v * v;
    }
    s_power += s_alpha * (sum / CHUNK_SAMPLES - s_power);

    const float db = s_power > 0.0f ? fmaxf(10.0f * log10f(s_power), LEVEL_FLOOR_DB) : LEVEL_FLOOR_DB;
    int old = atomic_load(&s_level_max_db);
    const int idb = (int)lroundf(db);
    while (idb > old && !atomic_compare_exchange_weak(&s_level_max_db, &old, idb)) {
    }

    if (s_warmup > 0) {
        s_warmup--;
        return false;
    }

    const float threshold = (float)CONFIG_APP_SOUND_THRESHOLD_DB;
    if (!s_armed) {
        s_armed = db < threshold - REARM_HYSTERESIS_DB;
        return false;
    }
    if (db >= threshold) {
        s_armed = false;
        atomic_fetch_add(&s_triggers, 1);
        ESP_LOGI(TAG, "响声触发：平滑音量 %.1f dB（阈值 %d）%s", db, CONFIG_APP_SOUND_THRESHOLD_DB,
                 CONFIG_APP_SOUND_TRIGGER_ENABLE ? "" : "，音量触发开关屏未启用，不切屏");
        return true;
    }
    return false;
}

#endif /* CONFIG_APP_VOICE_MODE_LEVEL */

static void voice_task(void *arg)
{
    static int16_t buf[CHUNK_SAMPLES];

    while (1) {
        if (mic_read(buf, CHUNK_SAMPLES) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(CHUNK_MS));
            continue;
        }
        track_peak(buf);

#if CONFIG_APP_VOICE_MODE_LEVEL
        /* 没启用也照样算音量，调试面板要看读数来调阈值 */
        if (level_feed(buf) && CONFIG_APP_SOUND_TRIGGER_ENABLE && s_cb) {
            s_cb(VOICE_SCREEN_TOGGLE);
        }
#else
        const kws_result_t result = kws_feed(buf, CHUNK_SAMPLES);
        if (result != KWS_NONE) {
            ESP_LOGI(TAG, "识别到：%s", result == KWS_SCREEN_ON ? "打开屏幕" : "关闭屏幕");
            if (s_cb) {
                s_cb(result == KWS_SCREEN_ON ? VOICE_SCREEN_ON : VOICE_SCREEN_OFF);
            }
        }
#endif
    }
}

esp_err_t voice_start(voice_cb_t on_cmd)
{
    s_cb = on_cmd;

    ESP_RETURN_ON_ERROR(mic_init(), TAG, "麦克风初始化失败");
#if CONFIG_APP_VOICE_MODE_KWS
    ESP_RETURN_ON_ERROR(kws_init(), TAG, "识别器初始化失败");
#endif

    ESP_RETURN_ON_FALSE(xTaskCreate(voice_task, "voice", VOICE_TASK_STACK, NULL, VOICE_TASK_PRIORITY, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "创建语音任务失败");

    s_running = true;
#if CONFIG_APP_VOICE_MODE_LEVEL
    ESP_LOGI(TAG, "音量触发开关屏%s：阈值 %d dB，平滑 %d ms，间隔 %d ms，增益 %ddB",
             CONFIG_APP_SOUND_TRIGGER_ENABLE ? "已启用" : "未启用（只算音量）", CONFIG_APP_SOUND_THRESHOLD_DB, CONFIG_APP_SOUND_SMOOTH_MS, CONFIG_APP_SOUND_COOLDOWN_MS, MIC_GAIN_DB);
#else
    ESP_LOGI(TAG, "关键词开关屏已启用，增益 %ddB", MIC_GAIN_DB);
#endif
    return ESP_OK;
}

bool voice_is_running(void)
{
    return s_running;
}

uint16_t voice_take_peak(void)
{
    const unsigned int peak = atomic_exchange(&s_peak, 0);
    return peak > 32767 ? 32767 : (uint16_t)peak;
}

int voice_take_level_max_db(void)
{
    return atomic_exchange(&s_level_max_db, (int)LEVEL_FLOOR_DB);
}

uint32_t voice_trigger_count(void)
{
    return atomic_load(&s_triggers);
}
