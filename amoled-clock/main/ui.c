#include "ui.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "board_pins.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fonts_sc.h"
#include "lvgl.h"

static const char *TAG = "ui";

/* ---- 版面 ----------------------------------------------------------
 * 536 x 240，从上到下：
 *   0   .. 44   雪花动画带（冷的时候才显示）
 *   46  .. 80   日期 + 星期
 *   82  .. 138  大号时钟
 *   146 .. 180  温湿度
 *   184 .. 240  火焰动画带（热的时候才显示）
 * 右上角是网络状态，亮度浮层在正中间。
 */
#define SNOW_BAND_H   44
#define FIRE_BAND_H   56
#define FIRE_BAND_Y   (LCD_V_RES - FIRE_BAND_H)

#define SNOW_FLAKES   22
#define FIRE_TONGUES  13

#define ANIM_PERIOD_MS       60
#define CLOCK_PERIOD_MS      200
#define BRIGHTNESS_HIDE_MS   1200

typedef enum {
    ANIM_NONE = 0,
    ANIM_SNOW,
    ANIM_FIRE,
} anim_mode_t;

static lv_obj_t *s_date_label;
static lv_obj_t *s_time_label;
static lv_obj_t *s_env_label;
static lv_obj_t *s_status_label;

static lv_obj_t   *s_snow_canvas;
static lv_obj_t   *s_fire_canvas;
static lv_color_t *s_snow_buf;
static lv_color_t *s_fire_buf;

static lv_obj_t *s_bright_panel;
static lv_obj_t *s_bright_bar;
static lv_obj_t *s_bright_label;

static lv_timer_t *s_bright_timer;

static anim_mode_t s_anim_mode = ANIM_NONE;
static uint32_t    s_anim_frame;

static int s_last_second = -1;

/* 雪花状态 */
static struct {
    int16_t x;
    int16_t y;
    int8_t  speed;
    int8_t  radius;
} s_flakes[SNOW_FLAKES];

/* ------------------------------------------------------------------ */
/* 动画                                                                */
/* ------------------------------------------------------------------ */

static void snow_reset_flake(int i, bool from_top)
{
    s_flakes[i].x = rand() % LCD_H_RES;
    s_flakes[i].y = from_top ? -(rand() % 12) : (rand() % SNOW_BAND_H);
    s_flakes[i].speed = 1 + rand() % 3;
    s_flakes[i].radius = 1 + rand() % 2;
}

static void snow_set_px(int x, int y, lv_color_t color)
{
    if (x < 0 || x >= LCD_H_RES || y < 0 || y >= SNOW_BAND_H) {
        return;
    }
    lv_canvas_set_px_color(s_snow_canvas, x, y, color);
}

static void draw_snow(void)
{
    const lv_color_t white = lv_color_hex(0xFFFFFF);
    const lv_color_t cyan  = lv_color_hex(0x7FD8FF);

    lv_canvas_fill_bg(s_snow_canvas, lv_color_black(), LV_OPA_COVER);

    for (int i = 0; i < SNOW_FLAKES; i++) {
        s_flakes[i].y += s_flakes[i].speed;
        /* 左右各飘一点，看起来不像下雨 */
        if ((s_anim_frame + i) % 8 == 0) {
            s_flakes[i].x += (i % 2) ? 1 : -1;
            if (s_flakes[i].x < 0) {
                s_flakes[i].x = LCD_H_RES - 1;
            } else if (s_flakes[i].x >= LCD_H_RES) {
                s_flakes[i].x = 0;
            }
        }
        if (s_flakes[i].y >= SNOW_BAND_H) {
            snow_reset_flake(i, true);
        }

        const int x = s_flakes[i].x;
        const int y = s_flakes[i].y;
        const int r = s_flakes[i].radius;

        /* 十字形的一片雪 */
        for (int d = -r; d <= r; d++) {
            snow_set_px(x + d, y, white);
            snow_set_px(x, y + d, white);
        }
        if (r == 2) {
            snow_set_px(x - 1, y - 1, cyan);
            snow_set_px(x + 1, y + 1, cyan);
            snow_set_px(x - 1, y + 1, cyan);
            snow_set_px(x + 1, y - 1, cyan);
        }
    }

    lv_obj_invalidate(s_snow_canvas);
}

static void draw_fire(void)
{
    const int bottom = FIRE_BAND_H - 1;

    lv_canvas_fill_bg(s_fire_canvas, lv_color_black(), LV_OPA_COVER);

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;

    for (int i = 0; i < FIRE_TONGUES; i++) {
        const int   center_x = i * (LCD_H_RES / FIRE_TONGUES) + 20;
        const float wave     = sinf(s_anim_frame * 0.28f + i * 1.37f);
        const int   outer_h  = 26 + (i * 7) % 12 + (int)(wave * 6);
        const int   inner_h  = outer_h * 2 / 3;

        /* 外焰 */
        dsc.bg_color = lv_color_hex(0xD01808);
        lv_point_t outer[3] = {
            {center_x - 20, bottom},
            {center_x + 20, bottom},
            {center_x + (int)(wave * 6), bottom - outer_h},
        };
        lv_canvas_draw_polygon(s_fire_canvas, outer, 3, &dsc);

        /* 内焰 */
        dsc.bg_color = lv_color_hex(0xF07818);
        lv_point_t inner[3] = {
            {center_x - 12, bottom},
            {center_x + 12, bottom},
            {center_x - (int)(wave * 4), bottom - inner_h},
        };
        lv_canvas_draw_polygon(s_fire_canvas, inner, 3, &dsc);
    }

    /* 焰心：贴着底边的一排黄点 */
    dsc.bg_color = lv_color_hex(0xF8D848);
    dsc.radius = LV_RADIUS_CIRCLE;
    for (int i = 0; i < FIRE_TONGUES; i++) {
        const int center_x = i * (LCD_H_RES / FIRE_TONGUES) + 20;
        lv_canvas_draw_rect(s_fire_canvas, center_x - 7, bottom - 10, 14, 14, &dsc);
    }
    dsc.radius = 0;

    lv_obj_invalidate(s_fire_canvas);
}

static void anim_timer_cb(lv_timer_t *timer)
{
    /* 关屏时不画，省得白白占用 CPU 和 SPI 带宽 */
    if (!display_is_on() || s_anim_mode == ANIM_NONE) {
        return;
    }

    s_anim_frame++;
    if (s_anim_mode == ANIM_SNOW) {
        draw_snow();
    } else {
        draw_fire();
    }
}

static void set_anim_mode(anim_mode_t mode)
{
    if (mode == s_anim_mode) {
        return;
    }
    s_anim_mode = mode;
    s_anim_frame = 0;

    if (mode == ANIM_SNOW) {
        for (int i = 0; i < SNOW_FLAKES; i++) {
            snow_reset_flake(i, false);
        }
        lv_obj_clear_flag(s_snow_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_fire_canvas, LV_OBJ_FLAG_HIDDEN);
    } else if (mode == ANIM_FIRE) {
        lv_obj_add_flag(s_snow_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_fire_canvas, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_snow_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_fire_canvas, LV_OBJ_FLAG_HIDDEN);
    }

    ESP_LOGI(TAG, "动画切换到 %s", mode == ANIM_SNOW ? "雪花" : (mode == ANIM_FIRE ? "火焰" : "无"));
}

/* ------------------------------------------------------------------ */
/* 时钟                                                                */
/* ------------------------------------------------------------------ */

static void clock_timer_cb(lv_timer_t *timer)
{
    if (!display_is_on()) {
        return;
    }

    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);

    if (local.tm_sec == s_last_second) {
        return;
    }
    s_last_second = local.tm_sec;

    /* tm_wday 从周日开始 */
    static const char *const weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};

    char buf[48];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d  周%s",
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, weekdays[local.tm_wday]);
    lv_label_set_text(s_date_label, buf);

    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
    lv_label_set_text(s_time_label, buf);
}

/* ------------------------------------------------------------------ */
/* 亮度浮层                                                            */
/* ------------------------------------------------------------------ */

static void brightness_hide_cb(lv_timer_t *timer)
{
    lv_obj_add_flag(s_bright_panel, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(s_bright_timer);
}

void ui_show_brightness(uint8_t brightness)
{
    if (!display_lock(200)) {
        return;
    }

    const int percent = (brightness - DISPLAY_BRIGHTNESS_MIN) * 100 /
                        (DISPLAY_BRIGHTNESS_MAX - DISPLAY_BRIGHTNESS_MIN);
    lv_bar_set_value(s_bright_bar, percent, LV_ANIM_OFF);

    char buf[24];
    snprintf(buf, sizeof(buf), "亮度 %d%%", percent);
    lv_label_set_text(s_bright_label, buf);

    lv_obj_clear_flag(s_bright_panel, LV_OBJ_FLAG_HIDDEN);
    lv_timer_reset(s_bright_timer);
    lv_timer_resume(s_bright_timer);

    display_unlock();
}

void ui_hide_brightness(void)
{
    if (!display_lock(200)) {
        return;
    }
    /* 手指抬起后再停留一会儿，让人看清最终档位 */
    lv_timer_reset(s_bright_timer);
    lv_timer_resume(s_bright_timer);
    display_unlock();
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

void ui_set_environment(bool ok, float temperature_c, float humidity_rh)
{
    if (!display_lock(500)) {
        return;
    }

    if (ok) {
        char buf[64];
        snprintf(buf, sizeof(buf), "温度 %.1f°C   湿度 %.0f%%", temperature_c, humidity_rh);
        lv_label_set_text(s_env_label, buf);
        lv_obj_set_style_text_color(s_env_label, lv_color_hex(0x63E6A0), LV_PART_MAIN);

        if (temperature_c > (float)CONFIG_APP_TEMP_HOT_THRESHOLD) {
            set_anim_mode(ANIM_FIRE);
        } else if (temperature_c < (float)CONFIG_APP_TEMP_COLD_THRESHOLD) {
            set_anim_mode(ANIM_SNOW);
        } else {
            set_anim_mode(ANIM_NONE);
        }
    } else {
        lv_label_set_text(s_env_label, "传感器离线");
        lv_obj_set_style_text_color(s_env_label, lv_color_hex(0xF0A030), LV_PART_MAIN);
        set_anim_mode(ANIM_NONE);
    }

    display_unlock();
}

void ui_set_net_state(net_state_t state)
{
    if (!display_lock(500)) {
        return;
    }

    switch (state) {
    case NET_STATE_CONNECTING:
        lv_label_set_text(s_status_label, "连接中");
        break;
    case NET_STATE_SYNCING:
        lv_label_set_text(s_status_label, "校时中");
        break;
    case NET_STATE_READY:
        /* 一切正常时不占地方 */
        lv_label_set_text(s_status_label, "");
        break;
    case NET_STATE_OFFLINE:
        lv_label_set_text(s_status_label, "离线");
        break;
    }

    display_unlock();
}

void ui_init(void)
{
    display_lock(-1);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 动画画布。缓冲同样放 PSRAM ---- */
    s_snow_buf = heap_caps_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(LCD_H_RES, SNOW_BAND_H),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_fire_buf = heap_caps_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(LCD_H_RES, FIRE_BAND_H),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(s_snow_buf && s_fire_buf);

    s_snow_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_snow_canvas, s_snow_buf, LCD_H_RES, SNOW_BAND_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(s_snow_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_canvas_fill_bg(s_snow_canvas, lv_color_black(), LV_OPA_COVER);
    lv_obj_add_flag(s_snow_canvas, LV_OBJ_FLAG_HIDDEN);

    s_fire_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_fire_canvas, s_fire_buf, LCD_H_RES, FIRE_BAND_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(s_fire_canvas, LV_ALIGN_TOP_LEFT, 0, FIRE_BAND_Y);
    lv_canvas_fill_bg(s_fire_canvas, lv_color_black(), LV_OPA_COVER);
    lv_obj_add_flag(s_fire_canvas, LV_OBJ_FLAG_HIDDEN);

    /* ---- 文字。画布先建，标签后建，这样标签压在动画之上 ---- */
    s_date_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_date_label, &font_sc_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_date_label, lv_color_hex(0x59C7F0), LV_PART_MAIN);
    lv_label_set_text(s_date_label, "");
    lv_obj_align(s_date_label, LV_ALIGN_TOP_MID, 0, 46);

    /* 大号时钟的字模里只有数字和冒号，占位符也只能用这些字符 */
    s_time_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_time_label, &font_sc_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_label_set_text(s_time_label, "00:00:00");
    lv_obj_align(s_time_label, LV_ALIGN_TOP_MID, 0, 82);

    s_env_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_env_label, &font_sc_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_env_label, lv_color_hex(0xF0A030), LV_PART_MAIN);
    lv_label_set_text(s_env_label, "传感器离线");
    lv_obj_align(s_env_label, LV_ALIGN_TOP_MID, 0, 146);

    s_status_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_status_label, &font_sc_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x808A94), LV_PART_MAIN);
    lv_label_set_text(s_status_label, "连接中");
    lv_obj_align(s_status_label, LV_ALIGN_TOP_RIGHT, -12, 8);

    /* ---- 亮度浮层 ---- */
    s_bright_panel = lv_obj_create(scr);
    lv_obj_set_size(s_bright_panel, 340, 68);
    lv_obj_align(s_bright_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_bright_panel, lv_color_hex(0x1A1F26), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bright_panel, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bright_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bright_panel, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_bright_panel, 16, LV_PART_MAIN);
    lv_obj_clear_flag(s_bright_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_bright_panel, LV_OBJ_FLAG_HIDDEN);

    s_bright_bar = lv_bar_create(s_bright_panel);
    lv_obj_set_size(s_bright_bar, 160, 14);
    lv_obj_align(s_bright_bar, LV_ALIGN_LEFT_MID, 0, 0);
    lv_bar_set_range(s_bright_bar, 0, 100);
    lv_obj_set_style_bg_color(s_bright_bar, lv_color_hex(0x39424D), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bright_bar, lv_color_hex(0xF8D848), LV_PART_INDICATOR);

    s_bright_label = lv_label_create(s_bright_panel);
    lv_obj_set_style_text_font(s_bright_label, &font_sc_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_bright_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_label_set_text(s_bright_label, "亮度 100%");
    lv_obj_align(s_bright_label, LV_ALIGN_RIGHT_MID, 0, 0);

    /* ---- 定时器 ---- */
    lv_timer_create(clock_timer_cb, CLOCK_PERIOD_MS, NULL);
    lv_timer_create(anim_timer_cb, ANIM_PERIOD_MS, NULL);

    s_bright_timer = lv_timer_create(brightness_hide_cb, BRIGHTNESS_HIDE_MS, NULL);
    lv_timer_set_repeat_count(s_bright_timer, -1);
    lv_timer_pause(s_bright_timer);

    display_unlock();
    ESP_LOGI(TAG, "界面就绪");
}
