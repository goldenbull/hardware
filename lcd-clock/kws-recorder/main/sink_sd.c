/*
 * 录音存 TF 卡：/sdcard/kws/open_001.wav …，每次录音在 sessions.csv 里记一行。
 */
#include "sink.h"

#if CONFIG_REC_STORAGE_SD

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "mic.h"
#include "sdcard.h"
#include "wav.h"

static const char *TAG = "sink_sd";

#define REC_DIR SDCARD_MOUNT_POINT "/kws"
#define REC_LOG REC_DIR "/sessions.csv"

static FILE *s_file;

esp_err_t sink_init(void)
{
    /* 没插卡也让界面起来，点开始录音时会再试一次挂载 */
    sdcard_mount();
    return ESP_OK;
}

/* 查剩余空间可能要扫整张 FAT 表，大卡上要好几秒，所以只在开机和每次录音前后调 */
void sink_refresh(char *text, size_t text_len, bool *ok)
{
    if (!sdcard_is_mounted()) {
        snprintf(text, text_len, "未检测到 TF 卡");
        *ok = false;
        return;
    }
    const uint32_t free_mb = (uint32_t)(sdcard_free_bytes() >> 20);
    snprintf(text, text_len, "TF 剩余 %lu.%lu G",
             (unsigned long)(free_mb / 1024), (unsigned long)(free_mb % 1024 * 10 / 1024));
    *ok = true;
}

/* 回写文件头里的长度并刷到卡上，之后文件指针回到末尾继续追加 */
static bool sync_header(uint32_t data_bytes)
{
    wav_header_t h;
    wav_fill_header(&h, data_bytes);
    if (fseek(s_file, 0, SEEK_SET) != 0 || fwrite(&h, sizeof(h), 1, s_file) != 1 ||
        fseek(s_file, 0, SEEK_END) != 0) {
        return false;
    }
    if (fflush(s_file) != 0) {
        return false;
    }
    return fsync(fileno(s_file)) == 0;
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

bool sink_open(const char *prefix, char *name, size_t name_len, const char **error)
{
    if (sdcard_mount() != ESP_OK) {
        *error = "TF 卡挂载失败";
        return false;
    }
    if (mkdir(REC_DIR, 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "创建 %s 失败：errno=%d", REC_DIR, errno);
        *error = "创建目录失败";
        return false;
    }

    snprintf(name, name_len, "%s_%03d.wav", prefix, next_index(prefix));

    char path[64];
    snprintf(path, sizeof(path), REC_DIR "/%s", name);

    s_file = fopen(path, "wb+");
    if (!s_file) {
        ESP_LOGE(TAG, "创建 %s 失败：errno=%d", path, errno);
        *error = "创建文件失败";
        return false;
    }
    if (!sync_header(0)) {
        fclose(s_file);
        s_file = NULL;
        *error = "写文件头失败";
        return false;
    }
    ESP_LOGI(TAG, "写入 %s", path);
    return true;
}

sink_result_t sink_write(const void *data, size_t len, uint32_t offset, const char **error)
{
    if (fwrite(data, 1, len, s_file) != len) {
        ESP_LOGE(TAG, "写卡失败：errno=%d", errno);
        *error = "写卡失败，录音已停止（卡满了？）";
        return SINK_FAIL;
    }
    return SINK_OK;
}

bool sink_checkpoint(uint32_t data_bytes)
{
    return sync_header(data_bytes);
}

static void append_log(const sink_log_t *log)
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
            log->file, log->keyword,
            (double)log->samples / MIC_SAMPLE_RATE,
            (unsigned long)log->samples,
            (unsigned long)log->dropped_samples,
            (unsigned long)log->clipped_samples,
            MIC_GAIN_DB, MIC_SAMPLE_RATE,
            (unsigned long)log->wall_ms);
    fclose(f);
}

void sink_close(uint32_t data_bytes, const sink_log_t *log)
{
    if (!s_file) {
        return;
    }
    /* 写失败（多半是卡满了）也尽量把已经写进去的部分收好尾 */
    sync_header(data_bytes);
    fclose(s_file);
    s_file = NULL;
    append_log(log);
}

#endif /* CONFIG_REC_STORAGE_SD */
