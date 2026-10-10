#include "touch.h"

#include <stdlib.h>

#include "board_pins.h"
#include "display.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

static const char *TAG = "touch";

#define TOUCH_POLL_MS      20

/* 手势判定阈值 */
#define TAP_MAX_MOVE_PX    20     /* 位移超过这个数就不算单击 */
#define TAP_MAX_MS         500    /* 按住超过这么久也不算单击 */
#define TOGGLE_COOLDOWN_MS 400    /* 上次开关屏之后这么久内的单击直接忽略 */
#define SWIPE_START_PX     28     /* 竖向超过这个数才进入调亮度模式 */
#define SWIPE_MAX_DX_PX    60     /* 横向偏太多说明不是竖滑 */
/* 竖向划过 180px ≈ 覆盖整个亮度范围（屏幕才 240 高，不能取太大） */
#define SWIPE_FULL_RANGE_PX 180

static touch_callbacks_t s_cb;

bool touch_read_point(uint16_t *x, uint16_t *y)
{
    uint8_t points = 0;
    if (i2c_bus_read_reg(I2C_ADDR_FT3168, 0x02, &points, 1) != ESP_OK || points == 0) {
        return false;
    }

    uint8_t buf[4];
    if (i2c_bus_read_reg(I2C_ADDR_FT3168, 0x03, buf, sizeof(buf)) != ESP_OK) {
        return false;
    }

    /* 寄存器里先 Y 后 X，各 12 位 */
    uint16_t ty = (((uint16_t)buf[0] & 0x0F) << 8) | buf[1];
    uint16_t tx = (((uint16_t)buf[2] & 0x0F) << 8) | buf[3];

    if (tx > LCD_H_RES) {
        tx = LCD_H_RES;
    }
    if (ty > LCD_V_RES) {
        ty = LCD_V_RES;
    }
    /* 面板在 MADCTL=0xF0 下是上下翻转的，和出厂固件保持一致 */
    ty = LCD_V_RES - ty;

    /*
     * 屏幕转了 180° 之后触摸面板本身没动，两个轴都要再翻一次，
     * 否则"往上滑"会变成往下调亮度。
     */
    if (display_is_flipped()) {
        tx = LCD_H_RES - tx;
        ty = LCD_V_RES - ty;
    }

    *x = tx;
    *y = ty;
    return true;
}

/*
 * 单击切换开关屏。
 * 单击只在抬起时判定，且要求全程位移 <= TAP_MAX_MOVE_PX；而进入调亮度要求竖向
 * 位移 >= SWIPE_START_PX，两者互斥，滑动不会误触开关屏。
 * 连点会让面板反复收到 0x28/0x29，跟不上就黑在那儿，所以两次切换之间强制隔开
 * TOGGLE_COOLDOWN_MS，冷却期内的单击直接丢掉（顺带让习惯性双击只切一次）。
 */
static void touch_task(void *arg)
{
    bool     pressed        = false;
    bool     swiping        = false;
    uint16_t start_x = 0, start_y = 0;
    uint16_t last_x = 0, last_y = 0;
    int      max_move       = 0;
    int64_t  press_time_us  = 0;
    int64_t  last_toggle_us = 0;       /* 上一次开关屏的时刻，用来做冷却 */
    uint8_t  start_bright   = DISPLAY_BRIGHTNESS_DEFAULT;

    while (1) {
        uint16_t x, y;
        const bool now_pressed = touch_read_point(&x, &y);

        if (now_pressed && !pressed) {
            /* 按下 */
            pressed = true;
            swiping = false;
            start_x = last_x = x;
            start_y = last_y = y;
            max_move = 0;
            press_time_us = esp_timer_get_time();
            start_bright = display_get_brightness();
        } else if (now_pressed && pressed) {
            /* 移动 */
            last_x = x;
            last_y = y;

            const int dx = (int)x - (int)start_x;
            const int dy = (int)y - (int)start_y;
            const int move = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
            if (move > max_move) {
                max_move = move;
            }

            /* 关屏状态下不接受滑动，避免摸黑乱调 */
            if (!swiping && display_is_on() && abs(dy) >= SWIPE_START_PX && abs(dx) < SWIPE_MAX_DX_PX) {
                swiping = true;
            }

            if (swiping) {
                const int span = DISPLAY_BRIGHTNESS_MAX - DISPLAY_BRIGHTNESS_MIN;
                /* y 轴向下为正，所以往上滑（dy 为负）要变亮，这里用减号 */
                int target = (int)start_bright - dy * span / SWIPE_FULL_RANGE_PX;
                if (target < DISPLAY_BRIGHTNESS_MIN) {
                    target = DISPLAY_BRIGHTNESS_MIN;
                } else if (target > DISPLAY_BRIGHTNESS_MAX) {
                    target = DISPLAY_BRIGHTNESS_MAX;
                }
                if (target != display_get_brightness()) {
                    display_set_brightness((uint8_t)target);
                    if (s_cb.on_brightness_change) {
                        s_cb.on_brightness_change((uint8_t)target);
                    }
                }
            }
        } else if (!now_pressed && pressed) {
            /* 抬起 */
            pressed = false;
            const int64_t held_ms = (esp_timer_get_time() - press_time_us) / 1000;

            if (swiping) {
                if (s_cb.on_brightness_end) {
                    s_cb.on_brightness_end();
                }
            } else if (max_move <= TAP_MAX_MOVE_PX && held_ms <= TAP_MAX_MS) {
                const int64_t now_us = esp_timer_get_time();
                if (last_toggle_us == 0 || (now_us - last_toggle_us) / 1000 >= TOGGLE_COOLDOWN_MS) {
                    display_set_on(!display_is_on());
                    /* 取 set_on 返回之后的时刻，冷却从面板命令发完才开始算 */
                    last_toggle_us = esp_timer_get_time();
                }
            }
            swiping = false;
        }

        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
}

esp_err_t touch_init(const touch_callbacks_t *callbacks)
{
    if (callbacks) {
        s_cb = *callbacks;
    }

    ESP_RETURN_ON_ERROR(i2c_bus_probe(I2C_ADDR_FT3168), TAG, "FT3168 无响应（不带触摸的板子会这样）");

    /* 写 0x00 寄存器切到正常工作模式 */
    const uint8_t mode = 0x00;
    ESP_RETURN_ON_ERROR(i2c_bus_write_reg(I2C_ADDR_FT3168, 0x00, &mode, 1), TAG, "FT3168 初始化失败");

    ESP_RETURN_ON_FALSE(xTaskCreate(touch_task, "touch", 3072, NULL, 3, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "创建触摸任务失败");

    ESP_LOGI(TAG, "触摸就绪：单击开关屏，上下滑动调亮度");
    return ESP_OK;
}
