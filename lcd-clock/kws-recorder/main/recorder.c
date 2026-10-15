#include "recorder.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "mic.h"
#include "sink.h"

static const char *TAG = "recorder";

/* 采集块 20ms，和 mic.c 里的 I2S DMA 帧长一致 */
#define CAP_CHUNK_SAMPLES   (MIC_SAMPLE_RATE / 50)
/* 16kHz * 2B = 32KB/s，4MB 能扛 2 分钟多的写卡停顿 / 断网 */
#define RING_BYTES          (4 * 1024 * 1024)
/* 攒够这么多（0.5 秒）就写一次。HTTP 下就是每个请求的大小 */
#define WRITE_CHUNK_BYTES   (16 * 1024)
/* 没攒够也最多隔这么久写一次，界面上的已落盘时长不至于一跳一跳的 */
#define FLUSH_PERIOD_MS     1000
/* 每隔这么久调一次 sink_checkpoint()：TF 卡回写 WAV 头并 fsync，中途断电最多丢这么多秒 */
#define SYNC_PERIOD_MS      5000
/* 写盘暂时失败（断网）时隔多久重试 */
#define RETRY_DELAY_MS      1000
/* 按了结束之后还传不完，最多再等这么久就放弃，已经传上去的部分照样保留 */
#define STOP_GIVE_UP_MS     60000
/* 停止时等采集任务把手里那一块送完再收尾，要大于一个采集块（20ms） */
#define STOP_SETTLE_MS      60

#define CAP_TASK_PRIO       6
#define WR_TASK_PRIO        4

typedef enum {
    CMD_START,
    CMD_STOP,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    keyword_t  keyword;
} cmd_t;

static const struct {
    const char *text;
    const char *prefix;
} s_keywords[KEYWORD_COUNT] = {
    [KEYWORD_OPEN]  = {"打开屏幕", "open"},
    [KEYWORD_CLOSE] = {"关闭屏幕", "close"},
};

static RingbufHandle_t s_ring;
static QueueHandle_t   s_cmd_queue;

/* 采集任务和写盘任务之间共享的标志 / 计数，单核上原子量足够 */
static atomic_bool     s_capturing;
static atomic_uint     s_peak;
static atomic_uint     s_captured;
static atomic_uint     s_dropped;
static atomic_uint     s_clipped;

/* 给界面看的状态，写盘任务改、界面任务读 */
static portMUX_TYPE    s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static rec_status_t    s_status;

/*
 * 写盘用的中转缓冲放内部 RAM：环形缓冲在 PSRAM 里，直接交给 SPI DMA 会被拆成一扇区
 * 一扇区地写。写盘暂时失败时数据就留在这里，下次原样再交一次。
 */
static uint8_t s_write_buf[WRITE_CHUNK_BYTES] __attribute__((aligned(4)));
static size_t  s_pending;

const char *recorder_keyword_text(keyword_t keyword)
{
    return keyword < KEYWORD_COUNT ? s_keywords[keyword].text : "";
}

static void set_state(rec_state_t state)
{
    taskENTER_CRITICAL(&s_status_lock);
    s_status.state = state;
    taskEXIT_CRITICAL(&s_status_lock);
}

static void set_error(const char *error)
{
    taskENTER_CRITICAL(&s_status_lock);
    s_status.error = error;
    taskEXIT_CRITICAL(&s_status_lock);
}

static void set_samples(uint32_t samples)
{
    taskENTER_CRITICAL(&s_status_lock);
    s_status.samples = samples;
    taskEXIT_CRITICAL(&s_status_lock);
}

void recorder_get_status(rec_status_t *out)
{
    taskENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    taskEXIT_CRITICAL(&s_status_lock);

    out->captured_samples = atomic_load(&s_captured);
    out->dropped_samples = atomic_load(&s_dropped);
    out->clipped_samples = atomic_load(&s_clipped);
    out->peak = (uint16_t)atomic_exchange(&s_peak, 0);
}

void recorder_start(keyword_t keyword)
{
    const cmd_t cmd = {.type = CMD_START, .keyword = keyword};
    xQueueSend(s_cmd_queue, &cmd, 0);
}

void recorder_stop(void)
{
    const cmd_t cmd = {.type = CMD_STOP};
    xQueueSend(s_cmd_queue, &cmd, 0);
}

/* ------------------------------------------------------------------------- */
/* 采集任务                                                                   */
/* ------------------------------------------------------------------------- */

static void capture_task(void *arg)
{
    static int16_t buf[CAP_CHUNK_SAMPLES];

    while (1) {
        if (mic_read(buf, CAP_CHUNK_SAMPLES) != ESP_OK) {
            ESP_LOGW(TAG, "麦克风读取失败");
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        uint32_t peak = 0;
        uint32_t clipped = 0;
        for (int i = 0; i < CAP_CHUNK_SAMPLES; i++) {
            const int32_t s = buf[i];
            const uint32_t a = (uint32_t)(s < 0 ? -s : s);
            if (a > peak) {
                peak = a;
            }
            if (a >= 32767) {
                clipped++;
            }
        }
        if (peak > 32767) {
            peak = 32767;   /* -32768 的绝对值 */
        }

        uint32_t old = atomic_load(&s_peak);
        while (peak > old && !atomic_compare_exchange_weak(&s_peak, &old, peak)) {
        }

        if (atomic_load(&s_capturing)) {
            if (xRingbufferSend(s_ring, buf, sizeof(buf), 0) == pdTRUE) {
                atomic_fetch_add(&s_captured, CAP_CHUNK_SAMPLES);
            } else {
                atomic_fetch_add(&s_dropped, CAP_CHUNK_SAMPLES);
            }
            atomic_fetch_add(&s_clipped, clipped);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* 写盘任务                                                                   */
/* ------------------------------------------------------------------------- */

/* 从环形缓冲往中转缓冲里补数据。最多等 timeout 等第一块，之后只取现成的 */
static void fill_pending(TickType_t timeout)
{
    while (s_pending < WRITE_CHUNK_BYTES) {
        size_t len = 0;
        void *item = xRingbufferReceiveUpTo(s_ring, &len, timeout, WRITE_CHUNK_BYTES - s_pending);
        if (!item) {
            return;
        }
        memcpy(s_write_buf + s_pending, item, len);
        vRingbufferReturnItem(s_ring, item);
        s_pending += len;
        timeout = 0;
    }
}

/* 丢掉环形缓冲和中转缓冲里残留的数据 */
static void flush_ring(void)
{
    size_t len;
    void *item;
    while ((item = xRingbufferReceiveUpTo(s_ring, &len, 0, RING_BYTES)) != NULL) {
        vRingbufferReturnItem(s_ring, item);
    }
    s_pending = 0;
}

/* 查存储状态可能很慢（扫 FAT 表 / 等服务器回包），所以只在写盘任务里、录音间隙做 */
static void refresh_storage(void)
{
    char text[sizeof(s_status.storage)];
    bool ok = false;
    sink_refresh(text, sizeof(text), &ok);

    taskENTER_CRITICAL(&s_status_lock);
    s_status.storage_ok = ok;
    strlcpy(s_status.storage, text, sizeof(s_status.storage));
    taskEXIT_CRITICAL(&s_status_lock);
}

/* 停止采集，等采集任务把手里那一块送进环形缓冲 */
static void stop_capture(void)
{
    set_state(REC_STOPPING);
    atomic_store(&s_capturing, false);
    vTaskDelay(pdMS_TO_TICKS(STOP_SETTLE_MS));
}

/*
 * 录一段：一直把环形缓冲里的数据交给 sink，直到收到停止命令、并且剩下的都写完。
 * 返回已落盘的字节数，*complete 表示是不是全部落盘了。
 */
static uint32_t record_session(bool *complete)
{
    const TickType_t start = xTaskGetTickCount();
    TickType_t last_flush = start;
    TickType_t last_sync = start;
    TickType_t stop_at = 0;
    bool stopping = false;
    bool retrying = false;
    uint32_t data_bytes = 0;

    *complete = false;
    while (1) {
        cmd_t cmd;
        if (!stopping && xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE && cmd.type == CMD_STOP) {
            stop_capture();
            stopping = true;
            stop_at = xTaskGetTickCount();
        }

        /* 上一块还没交出去（在重试）的时候也继续往后补，补满为止 */
        fill_pending(stopping || retrying ? 0 : pdMS_TO_TICKS(100));
        if (stopping && s_pending == 0) {
            *complete = true;    /* 停了，而且全写完了 */
            break;
        }

        const TickType_t now = xTaskGetTickCount();
        const bool due = s_pending == WRITE_CHUNK_BYTES || stopping ||
                         now - last_flush >= pdMS_TO_TICKS(FLUSH_PERIOD_MS);
        if (!due || s_pending == 0) {
            continue;
        }

        const char *error = NULL;
        const sink_result_t r = sink_write(s_write_buf, s_pending, data_bytes, &error);
        if (r == SINK_OK) {
            data_bytes += s_pending;
            s_pending = 0;
            last_flush = now;
            set_samples(data_bytes / sizeof(int16_t));
            if (retrying) {
                retrying = false;
                set_error(NULL);
                ESP_LOGI(TAG, "恢复写入，已落盘 %lu 字节", (unsigned long)data_bytes);
            }
        } else if (r == SINK_FAIL) {
            set_error(error);
            break;
        } else {
            /* 断网之类：数据留在中转缓冲里，采集照常进环形缓冲，等会儿再试 */
            if (!retrying) {
                ESP_LOGW(TAG, "写入暂时失败：%s", error);
            }
            retrying = true;
            set_error(error);
            if (stopping && xTaskGetTickCount() - stop_at >= pdMS_TO_TICKS(STOP_GIVE_UP_MS)) {
                set_error("传不上去，后面一段没保存");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            continue;
        }

        if (xTaskGetTickCount() - last_sync >= pdMS_TO_TICKS(SYNC_PERIOD_MS)) {
            if (!sink_checkpoint(data_bytes)) {
                set_error("写文件头失败，录音已停止");
                break;
            }
            last_sync = xTaskGetTickCount();
        }
    }

    if (!stopping) {
        stop_capture();
    }
    flush_ring();
    return data_bytes;
}

static void writer_task(void *arg)
{
    refresh_storage();

    while (1) {
        cmd_t cmd;
        if (xQueueReceive(s_cmd_queue, &cmd, SINK_POLL_TICKS) != pdTRUE) {
            refresh_storage();   /* HTTP 模式下定期看看服务器还在不在 */
            continue;
        }
        if (cmd.type != CMD_START) {
            continue;   /* 空闲时收到停止，忽略 */
        }

        set_state(REC_STARTING);
        set_error(NULL);

        char name[sizeof(s_status.file)];
        const char *error = NULL;
        const bool opened = sink_open(s_keywords[cmd.keyword].prefix, name, sizeof(name), &error);
        refresh_storage();   /* 开机没插卡、这次才挂上的话，界面要跟着更新 */
        if (!opened) {
            set_error(error);
            set_state(REC_IDLE);
            continue;
        }

        taskENTER_CRITICAL(&s_status_lock);
        s_status.keyword = cmd.keyword;
        strlcpy(s_status.file, name, sizeof(s_status.file));
        s_status.samples = 0;
        taskEXIT_CRITICAL(&s_status_lock);
        ESP_LOGI(TAG, "开始录音：%s（%s）", name, s_keywords[cmd.keyword].text);

        flush_ring();
        atomic_store(&s_captured, 0);
        atomic_store(&s_dropped, 0);
        atomic_store(&s_clipped, 0);
        atomic_store(&s_capturing, true);
        set_state(REC_RECORDING);

        const TickType_t start = xTaskGetTickCount();
        bool complete = false;
        const uint32_t data_bytes = record_session(&complete);

        const sink_log_t log = {
            .file = name,
            .keyword = s_keywords[cmd.keyword].prefix,
            .samples = data_bytes / sizeof(int16_t),
            .dropped_samples = atomic_load(&s_dropped),
            .clipped_samples = atomic_load(&s_clipped),
            .wall_ms = pdTICKS_TO_MS(xTaskGetTickCount() - start),
            .complete = complete,
        };
        sink_close(data_bytes, &log);

        ESP_LOGI(TAG, "录音结束：%s，落盘 %.1f 秒（录了 %.1f 秒），丢帧 %lu，削顶 %lu%s",
                 name, (double)log.samples / MIC_SAMPLE_RATE,
                 (double)atomic_load(&s_captured) / MIC_SAMPLE_RATE,
                 (unsigned long)log.dropped_samples, (unsigned long)log.clipped_samples,
                 complete ? "" : "，不完整");
        refresh_storage();
        set_state(REC_IDLE);
    }
}

esp_err_t recorder_init(void)
{
    ESP_RETURN_ON_ERROR(sink_init(), TAG, "存储初始化失败");

    s_ring = xRingbufferCreateWithCaps(RING_BYTES, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_ring, ESP_ERR_NO_MEM, TAG, "环形缓冲分配失败（PSRAM 没开？）");

    s_cmd_queue = xQueueCreate(4, sizeof(cmd_t));
    ESP_RETURN_ON_FALSE(s_cmd_queue, ESP_ERR_NO_MEM, TAG, "命令队列创建失败");

    ESP_RETURN_ON_FALSE(xTaskCreate(capture_task, "rec_cap", 3072, NULL, CAP_TASK_PRIO, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "创建采集任务失败");
    /* HTTP 客户端一路调进 lwIP，栈比只写 FATFS 时深，给足 */
    ESP_RETURN_ON_FALSE(xTaskCreate(writer_task, "rec_wr", 8192, NULL, WR_TASK_PRIO, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "创建写盘任务失败");
    return ESP_OK;
}
