#include "recorder.h"

#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "mic.h"
#include "sdcard.h"

static const char *TAG = "recorder";

#define REC_DIR             SDCARD_MOUNT_POINT "/kws"
#define REC_LOG             REC_DIR "/sessions.csv"

/* 采集块 20ms，和 mic.c 里的 I2S DMA 帧长一致 */
#define CAP_CHUNK_SAMPLES   (MIC_SAMPLE_RATE / 50)
/* 16kHz * 2B = 32KB/s，512KB 能扛 16 秒的写卡停顿 */
#define RING_BYTES          (512 * 1024)
/* 每次最多从环形缓冲取这么多写卡，16 个扇区 */
#define WRITE_CHUNK_BYTES   (8 * 1024)
/* 每隔这么久回写一次 WAV 头并 fsync，中途断电最多丢这么多秒，文件头也始终是对的 */
#define SYNC_PERIOD_MS      5000
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

/* 采集任务和写卡任务之间共享的标志 / 计数，单核上原子量足够 */
static atomic_bool     s_capturing;
static atomic_uint     s_peak;
static atomic_uint     s_dropped;
static atomic_uint     s_clipped;

/* 给界面看的状态，写卡任务改、界面任务读 */
static portMUX_TYPE    s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static rec_status_t    s_status;

/* 写卡用的中转缓冲放内部 RAM：环形缓冲在 PSRAM 里，直接交给 SPI DMA 会被拆成一扇区一扇区地写 */
static uint8_t s_write_buf[WRITE_CHUNK_BYTES] __attribute__((aligned(4)));

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

void recorder_get_status(rec_status_t *out)
{
    taskENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    taskEXIT_CRITICAL(&s_status_lock);

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
            if (xRingbufferSend(s_ring, buf, sizeof(buf), 0) != pdTRUE) {
                atomic_fetch_add(&s_dropped, CAP_CHUNK_SAMPLES);
            }
            atomic_fetch_add(&s_clipped, clipped);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* 写卡任务                                                                   */
/* ------------------------------------------------------------------------- */

static void fill_header(wav_header_t *h, uint32_t data_bytes)
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

/* 回写文件头里的长度并刷到卡上，之后文件指针回到末尾继续追加 */
static bool sync_header(FILE *f, uint32_t data_bytes)
{
    wav_header_t h;
    fill_header(&h, data_bytes);
    if (fseek(f, 0, SEEK_SET) != 0 || fwrite(&h, sizeof(h), 1, f) != 1 || fseek(f, 0, SEEK_END) != 0) {
        return false;
    }
    if (fflush(f) != 0) {
        return false;
    }
    return fsync(fileno(f)) == 0;
}

/* 找 prefix_NNN.wav 里最大的 NNN，返回下一个编号 */
static int next_index(const char *prefix)
{
    const size_t plen = strlen(prefix);
    int max_idx = 0;

    DIR *dir = opendir(REC_DIR);
    if (!dir) {
        return 1;
    }
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        /* FAT 不区分大小写，按不区分大小写比较，免得在 Mac 上改过名的文件被忽略 */
        if (strncasecmp(e->d_name, prefix, plen) == 0 && e->d_name[plen] == '_') {
            const int idx = atoi(e->d_name + plen + 1);
            if (idx > max_idx) {
                max_idx = idx;
            }
        }
    }
    closedir(dir);
    return max_idx + 1;
}

static void append_log(const rec_status_t *st, uint32_t elapsed_ms)
{
    struct stat sb;
    const bool exists = stat(REC_LOG, &sb) == 0;

    FILE *f = fopen(REC_LOG, "a");
    if (!f) {
        ESP_LOGW(TAG, "打开 %s 失败：errno=%d", REC_LOG, errno);
        return;
    }
    if (!exists) {
        fprintf(f, "file,keyword,duration_s,samples,dropped_samples,clipped_samples,gain_db,sample_rate,wall_ms\n");
    }
    fprintf(f, "%s,%s,%.2f,%lu,%lu,%lu,%d,%d,%lu\n",
            st->file, s_keywords[st->keyword].prefix,
            (double)st->samples / MIC_SAMPLE_RATE,
            (unsigned long)st->samples,
            (unsigned long)atomic_load(&s_dropped),
            (unsigned long)atomic_load(&s_clipped),
            MIC_GAIN_DB, MIC_SAMPLE_RATE,
            (unsigned long)elapsed_ms);
    fclose(f);
}

/* 把环形缓冲里现有的数据全部写进文件。timeout 为 0 时只取已有的，不等 */
static bool drain_ring(FILE *f, uint32_t *data_bytes, TickType_t timeout)
{
    while (1) {
        size_t len = 0;
        void *item = xRingbufferReceiveUpTo(s_ring, &len, timeout, WRITE_CHUNK_BYTES);
        if (!item) {
            return true;
        }
        memcpy(s_write_buf, item, len);
        vRingbufferReturnItem(s_ring, item);

        if (fwrite(s_write_buf, 1, len, f) != len) {
            ESP_LOGE(TAG, "写卡失败：errno=%d", errno);
            return false;
        }
        *data_bytes += len;

        taskENTER_CRITICAL(&s_status_lock);
        s_status.samples = *data_bytes / sizeof(int16_t);
        taskEXIT_CRITICAL(&s_status_lock);

        if (timeout != 0) {
            return true;   /* 正常录音时一次只取一块，回去看看有没有停止命令 */
        }
    }
}

/* 丢掉环形缓冲里残留的数据 */
static void flush_ring(void)
{
    size_t len;
    void *item;
    while ((item = xRingbufferReceiveUpTo(s_ring, &len, 0, RING_BYTES)) != NULL) {
        vRingbufferReturnItem(s_ring, item);
    }
}

/* 建文件、写占位头。成功返回打开的 FILE*，失败返回 NULL 并设好 error */
static FILE *open_session(keyword_t keyword)
{
    if (sdcard_mount() != ESP_OK) {
        set_error("TF 卡挂载失败");
        return NULL;
    }
    if (mkdir(REC_DIR, 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "创建 %s 失败：errno=%d", REC_DIR, errno);
        set_error("创建目录失败");
        return NULL;
    }

    char name[sizeof(s_status.file)];
    snprintf(name, sizeof(name), "%s_%03d.wav", s_keywords[keyword].prefix, next_index(s_keywords[keyword].prefix));

    char path[64];
    snprintf(path, sizeof(path), REC_DIR "/%s", name);

    FILE *f = fopen(path, "wb+");
    if (!f) {
        ESP_LOGE(TAG, "创建 %s 失败：errno=%d", path, errno);
        set_error("创建文件失败");
        return NULL;
    }
    if (!sync_header(f, 0)) {
        fclose(f);
        set_error("写文件头失败");
        return NULL;
    }

    taskENTER_CRITICAL(&s_status_lock);
    s_status.keyword = keyword;
    strlcpy(s_status.file, name, sizeof(s_status.file));
    s_status.samples = 0;
    s_status.error = NULL;
    taskEXIT_CRITICAL(&s_status_lock);

    ESP_LOGI(TAG, "开始录音：%s（%s）", path, s_keywords[keyword].text);
    return f;
}

/* 查剩余空间可能要扫整张 FAT 表，大卡上要好几秒，所以只在写卡任务里、录音间隙做 */
static void update_free_space(void)
{
    const bool mounted = sdcard_is_mounted();
    const uint32_t free_mb = mounted ? (uint32_t)(sdcard_free_bytes() >> 20) : 0;

    taskENTER_CRITICAL(&s_status_lock);
    s_status.sd_mounted = mounted;
    s_status.sd_free_mb = free_mb;
    taskEXIT_CRITICAL(&s_status_lock);
}

static void writer_task(void *arg)
{
    update_free_space();

    while (1) {
        cmd_t cmd;
        xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY);
        if (cmd.type != CMD_START) {
            continue;   /* 空闲时收到停止，忽略 */
        }

        set_state(REC_STARTING);
        set_error(NULL);
        FILE *f = open_session(cmd.keyword);
        update_free_space();   /* 开机没插卡、这次才挂上的话，界面要跟着更新 */
        if (!f) {
            set_state(REC_IDLE);
            continue;
        }

        flush_ring();
        atomic_store(&s_dropped, 0);
        atomic_store(&s_clipped, 0);
        atomic_store(&s_capturing, true);
        set_state(REC_RECORDING);

        const TickType_t start = xTaskGetTickCount();
        TickType_t last_sync = start;
        uint32_t data_bytes = 0;
        bool ok = true;

        while (ok) {
            if (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE && cmd.type == CMD_STOP) {
                break;
            }
            ok = drain_ring(f, &data_bytes, pdMS_TO_TICKS(100));
            if (ok && xTaskGetTickCount() - last_sync >= pdMS_TO_TICKS(SYNC_PERIOD_MS)) {
                ok = sync_header(f, data_bytes);
                last_sync = xTaskGetTickCount();
            }
        }

        set_state(REC_STOPPING);
        atomic_store(&s_capturing, false);
        vTaskDelay(pdMS_TO_TICKS(STOP_SETTLE_MS));
        if (ok) {
            ok = drain_ring(f, &data_bytes, 0);
        }
        flush_ring();

        /* 写失败（多半是卡满了）也尽量把已经写进去的部分收好尾 */
        if (!sync_header(f, data_bytes)) {
            ok = false;
        }
        fclose(f);
        if (!ok) {
            set_error("写卡失败，录音已停止（卡满了？）");
        }

        rec_status_t st;
        recorder_get_status(&st);
        append_log(&st, pdTICKS_TO_MS(xTaskGetTickCount() - start));

        ESP_LOGI(TAG, "录音结束：%s，%.1f 秒，丢帧 %lu，削顶 %lu",
                 st.file, (double)st.samples / MIC_SAMPLE_RATE,
                 (unsigned long)st.dropped_samples, (unsigned long)st.clipped_samples);
        update_free_space();
        set_state(REC_IDLE);
    }
}

esp_err_t recorder_init(void)
{
    s_ring = xRingbufferCreateWithCaps(RING_BYTES, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_ring, ESP_ERR_NO_MEM, TAG, "环形缓冲分配失败（PSRAM 没开？）");

    s_cmd_queue = xQueueCreate(4, sizeof(cmd_t));
    ESP_RETURN_ON_FALSE(s_cmd_queue, ESP_ERR_NO_MEM, TAG, "命令队列创建失败");

    ESP_RETURN_ON_FALSE(xTaskCreate(capture_task, "rec_cap", 3072, NULL, CAP_TASK_PRIO, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "创建采集任务失败");
    ESP_RETURN_ON_FALSE(xTaskCreate(writer_task, "rec_wr", 6144, NULL, WR_TASK_PRIO, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "创建写卡任务失败");
    return ESP_OK;
}
