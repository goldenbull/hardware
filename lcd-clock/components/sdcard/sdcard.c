#include "sdcard.h"

#include "board_pins.h"
#include "driver/sdspi_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sdcard";

static sdmmc_card_t *s_card;

esp_err_t sdcard_mount(void)
{
    if (s_card) {
        return ESP_OK;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI_HOST_SHARED;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_NUM_SD_CS;
    slot.host_id = SPI_HOST_SHARED;

    /*
     * 录音文件只追加写，簇大一点能少更新几次 FAT 表。
     * 不自动格式化：挂载失败多半是卡没插好或者 JP8 没拨，格式化只会把别人的卡清掉。
     */
    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 32 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdspi_mount(SDCARD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        ESP_LOGE(TAG, "TF 卡挂载失败（%s）：检查卡是否插好、是否 FAT32、背面 JP8 第 2 位是否拨到 ON",
                 esp_err_to_name(err));
        return err;
    }

    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "TF 卡已挂载到 %s，剩余 %llu MB", SDCARD_MOUNT_POINT, sdcard_free_bytes() >> 20);
    return ESP_OK;
}

bool sdcard_is_mounted(void)
{
    return s_card != NULL;
}

uint64_t sdcard_free_bytes(void)
{
    if (!s_card) {
        return 0;
    }
    uint64_t total = 0, free_bytes = 0;
    if (esp_vfs_fat_info(SDCARD_MOUNT_POINT, &total, &free_bytes) != ESP_OK) {
        return 0;
    }
    return free_bytes;
}
