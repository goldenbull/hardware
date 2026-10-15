#include "touch.h"

#include <stdbool.h>

#include "board_pins.h"
#include "display_priv.h"
#include "esp_check.h"
#include "esp_log.h"
#include "i2c_bus.h"
#include "lvgl.h"

static const char *TAG = "touch";

#define FT6336_REG_TD_STATUS 0x02   /* 低 4 位是触点数 */
#define FT6336_REG_P1_XH     0x03   /* 接着依次是 P1_XL、P1_YH、P1_YL */

bool touch_read_point(uint16_t *x, uint16_t *y)
{
    uint8_t points = 0;
    if (i2c_bus_read_reg(I2C_ADDR_FT6336, FT6336_REG_TD_STATUS, &points, 1) != ESP_OK) {
        return false;
    }
    points &= 0x0F;
    /* 0 是没按；超过 2 是 FT6336 上电后寄存器还没就绪时读到的垃圾值 */
    if (points == 0 || points > 2) {
        return false;
    }

    uint8_t buf[4];
    if (i2c_bus_read_reg(I2C_ADDR_FT6336, FT6336_REG_P1_XH, buf, sizeof(buf)) != ESP_OK) {
        return false;
    }

    /* 先 X 后 Y，各 12 位，高字节的高 4 位是事件标志 */
    uint16_t tx = (((uint16_t)buf[0] & 0x0F) << 8) | buf[1];
    uint16_t ty = (((uint16_t)buf[2] & 0x0F) << 8) | buf[3];

    if (tx >= LCD_H_RES) {
        tx = LCD_H_RES - 1;
    }
    if (ty >= LCD_V_RES) {
        ty = LCD_V_RES - 1;
    }
    display_map_touch(tx, ty, x, y);
    return true;
}

/* LVGL 每 LV_INDEV_DEF_READ_PERIOD（默认 30ms）在 lvgl 任务里调一次 */
static void lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    static uint16_t last_x, last_y;
    uint16_t x, y;

    if (touch_read_point(&x, &y)) {
        last_x = x;
        last_y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    /* 松手时也要报最后的坐标，LVGL 靠它判断是不是在按钮内抬起的 */
    data->point.x = last_x;
    data->point.y = last_y;
}

esp_err_t touch_init(bool lvgl_indev)
{
    ESP_RETURN_ON_ERROR(i2c_bus_probe(I2C_ADDR_FT6336), TAG, "FT6336 无响应");

    /* 写 0x00 寄存器切到正常工作模式 */
    const uint8_t mode = 0x00;
    ESP_RETURN_ON_ERROR(i2c_bus_write_reg(I2C_ADDR_FT6336, 0x00, &mode, 1), TAG, "FT6336 初始化失败");

    if (!lvgl_indev) {
        ESP_LOGI(TAG, "触摸就绪（轮询模式）");
        return ESP_OK;
    }

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_read_cb;
    ESP_RETURN_ON_FALSE(lv_indev_drv_register(&indev_drv), ESP_FAIL, TAG, "注册 LVGL 输入设备失败");

    ESP_LOGI(TAG, "触摸就绪（LVGL 输入设备）");
    return ESP_OK;
}
