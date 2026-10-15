#include "ui.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fonts_sc.h"
#include "lvgl.h"

static const char *TAG = "ui";

/* ---- 版面 ----------------------------------------------------------
 * 横屏 480 x 320，从上到下：
 *   雪花动画带（冷的时候才有，0..44）
 *   日期
 *   大号时钟
 *   温湿度
 *   火焰动画带（热的时候才有，264..320）
 * 比 amoled 的 536x240 窄一点、高很多，所以日期和温湿度不再挤在同一行，
 * 三行放进一个竖排容器，按当前动画留出的空间整体居中，见下面的 s_regions。
 * 右上角是网络状态，左侧竖条是调亮度时的指示条。
 */
#define SCR_W         480
#define SCR_H         320

#define SNOW_BAND_H   44
#define FIRE_BAND_H   56
#define FIRE_BAND_Y   (SCR_H - FIRE_BAND_H)

/* 三行之间的间隔 */
#define ROW_GAP 14

/*
 * DSEG7 自带的字距很松，和 amoled 一样收紧一点：96 号收 5px 后 "00:00:00"
 * 着墨宽约 455px，左右各留十来像素。
 */
#define CLOCK_LETTER_SPACE (-5)

/* 亮度指示条：左侧竖条，垂直居中。贴边放，避开时钟从 x≈12 开始的第一个数字 */
#define BRIGHT_BAR_W 10
#define BRIGHT_BAR_H 160
#define BRIGHT_BAR_X 2    /* 距左边缘 */

/* 六角雪花画得大，片数就要少，否则 44px 高的带子里会糊成一片 */
#define SNOW_FLAKES   7
#define SNOW_R_MIN    5
#define SNOW_R_MAX    8
#define FIRE_TONGUES  12

#define ANIM_PERIOD_MS       60
#define CLOCK_PERIOD_MS      200
#define BRIGHTNESS_HIDE_MS   1200

typedef enum {
    ANIM_NONE = 0,
    ANIM_SNOW,
    ANIM_FIRE,
} anim_mode_t;

static lv_obj_t *s_content;
static lv_obj_t *s_date_label;
static lv_obj_t *s_time_label;
static lv_obj_t *s_env_label;
static lv_obj_t *s_status_label;

static lv_obj_t   *s_snow_canvas;
static lv_obj_t   *s_fire_canvas;
static lv_color_t *s_snow_buf;
static lv_color_t *s_fire_buf;

static lv_obj_t *s_bright_bar;

static lv_timer_t *s_bright_timer;

static anim_mode_t s_anim_mode = ANIM_NONE;
static uint32_t    s_anim_frame;

static int s_last_second = -1;

/*
 * 每种动画下三行文字可用的纵向区间，内容整体在区间里居中。
 * 三行的行高 43 + 96 + 43 加两个间隔约 210px：
 *   无动画：整屏 0..320
 *   雪花：雪花带底到屏底 44..320
 *   火焰：火焰带从 264 开始，但火苗最高 43px，尖顶到不了 272 以上；
 *         顶上让出右上角的网络状态（8..27），从 28 开始
 */
typedef struct {
    int16_t top;
    int16_t bottom;
} region_t;

static const region_t s_regions[] = {
    [ANIM_NONE] = {.top = 0,  .bottom = SCR_H},
    [ANIM_SNOW] = {.top = SNOW_BAND_H, .bottom = SCR_H},
    [ANIM_FIRE] = {.top = 28, .bottom = 272},
};

/* 雪花状态。x 是摆动的中心线，实际横坐标由 phase 正弦调制出来 */
static struct {
    int16_t x;
    int16_t y;
    int8_t  speed;
    int8_t  radius;
    uint8_t sway_amp;    /* 摆幅，像素 */
    uint8_t sway_step;   /* 每帧相位推进，决定摆动快慢 */
    uint8_t phase;       /* 0..255 对应 0..2π，溢出即自然回绕 */
} s_flakes[SNOW_FLAKES];

/* ------------------------------------------------------------------ */
/* 动画                                                                */
/* ------------------------------------------------------------------ */

static void snow_reset_flake(int i, bool from_top)
{
    const int r = SNOW_R_MIN + rand() % (SNOW_R_MAX - SNOW_R_MIN + 1);

    s_flakes[i].x = rand() % SCR_W;
    /* 从顶上重新飘下来时先整片藏在带子外面，免得凭空冒出来 */
    s_flakes[i].y = from_top ? -(r + rand() % 12) : (rand() % SNOW_BAND_H);
    s_flakes[i].speed = 1 + rand() % 2;
    s_flakes[i].radius = r;
    s_flakes[i].sway_amp = 4 + rand() % 7;
    s_flakes[i].sway_step = 3 + rand() % 5;
    s_flakes[i].phase = rand() % 256;
}

static void snow_set_px(int x, int y, lv_color_t color)
{
    if (x < 0 || x >= SCR_W || y < 0 || y >= SNOW_BAND_H) {
        return;
    }
    lv_canvas_set_px_color(s_snow_canvas, x, y, color);
}

/* 六个主枝的方向，间隔 60°：cos/sin(0°,60°,...,300°) */
static const float SNOW_DIR_X[6] = { 1.000f,  0.500f, -0.500f, -1.000f, -0.500f,  0.500f};
static const float SNOW_DIR_Y[6] = { 0.000f,  0.866f,  0.866f,  0.000f, -0.866f, -0.866f};

/* 从 (cx,cy) 沿单位向量画一条长 len 的枝 */
static void snow_draw_ray(int cx, int cy, float dx, float dy, int len, lv_color_t color)
{
    for (int t = 1; t <= len; t++) {
        snow_set_px(cx + (int)lroundf(dx * t), cy + (int)lroundf(dy * t), color);
    }
}

/*
 * 一片六角雪花：6 条主枝，每条主枝在 60% 处朝 ±60° 各分出一根小枝。
 * 小枝方向正好和相邻主枝同向，所以直接复用方向表。
 */
static void snow_draw_flake(int cx, int cy, int r, lv_color_t body, lv_color_t branch)
{
    snow_set_px(cx, cy, body);

    for (int a = 0; a < 6; a++) {
        snow_draw_ray(cx, cy, SNOW_DIR_X[a], SNOW_DIR_Y[a], r, body);

        const int bx = cx + (int)lroundf(SNOW_DIR_X[a] * r * 0.6f);
        const int by = cy + (int)lroundf(SNOW_DIR_Y[a] * r * 0.6f);
        const int blen = r / 3 + 1;
        snow_draw_ray(bx, by, SNOW_DIR_X[(a + 1) % 6], SNOW_DIR_Y[(a + 1) % 6], blen, branch);
        snow_draw_ray(bx, by, SNOW_DIR_X[(a + 5) % 6], SNOW_DIR_Y[(a + 5) % 6], blen, branch);
    }
}

static void draw_snow(void)
{
    const lv_color_t white = lv_color_hex(0xFFFFFF);
    const lv_color_t cyan  = lv_color_hex(0x7FD8FF);

    lv_canvas_fill_bg(s_snow_canvas, lv_color_black(), LV_OPA_COVER);

    for (int i = 0; i < SNOW_FLAKES; i++) {
        const int r = s_flakes[i].radius;

        s_flakes[i].y += s_flakes[i].speed;
        s_flakes[i].phase += s_flakes[i].sway_step;

        /* 整片都掉出带子下沿了才回收 */
        if (s_flakes[i].y - r >= SNOW_BAND_H) {
            snow_reset_flake(i, true);
            continue;
        }

        /*
         * 横向按正弦摆，配上匀速下落就是一条来回飘的曲线，
         * 而不是原来那种每 8 帧硬拐一格的折线。
         */
        const float rad = s_flakes[i].phase * (6.2831853f / 256.0f);
        const int   x   = s_flakes[i].x + (int)lroundf(s_flakes[i].sway_amp * sinf(rad));

        snow_draw_flake(x, s_flakes[i].y, r, white, cyan);
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
        const int   center_x = i * (SCR_W / FIRE_TONGUES) + 20;
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
        const int center_x = i * (SCR_W / FIRE_TONGUES) + 20;
        lv_canvas_draw_rect(s_fire_canvas, center_x - 7, bottom - 10, 14, 14, &dsc);
    }
    dsc.radius = 0;

    lv_obj_invalidate(s_fire_canvas);
}

static void anim_timer_cb(lv_timer_t *timer)
{
    /* 关屏、或者切到了调试面板时不画，省得白白占用 CPU 和 SPI 带宽 */
    if (!display_is_on() || s_anim_mode == ANIM_NONE || lv_obj_get_screen(s_snow_canvas) != lv_scr_act()) {
        return;
    }

    s_anim_frame++;
    if (s_anim_mode == ANIM_SNOW) {
        draw_snow();
    } else {
        draw_fire();
    }
}

static void apply_layout(anim_mode_t mode)
{
    const int center = (s_regions[mode].top + s_regions[mode].bottom) / 2;
    lv_obj_align(s_content, LV_ALIGN_CENTER, 0, center - SCR_H / 2);
}

static void set_anim_mode(anim_mode_t mode)
{
    if (mode == s_anim_mode) {
        return;
    }
    s_anim_mode = mode;
    s_anim_frame = 0;
    apply_layout(mode);

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
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d 周%s",
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
    lv_obj_add_flag(s_bright_bar, LV_OBJ_FLAG_HIDDEN);
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

    lv_obj_clear_flag(s_bright_bar, LV_OBJ_FLAG_HIDDEN);
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
        snprintf(buf, sizeof(buf), "%.1f°C %.0f%%", temperature_c, humidity_rh);
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

    /* ---- 动画画布。缓冲放 PSRAM，内部 RAM 留给 LVGL 的 DMA 绘制缓冲 ---- */
    s_snow_buf = heap_caps_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(SCR_W, SNOW_BAND_H),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_fire_buf = heap_caps_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(SCR_W, FIRE_BAND_H),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(s_snow_buf && s_fire_buf);

    s_snow_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_snow_canvas, s_snow_buf, SCR_W, SNOW_BAND_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(s_snow_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_canvas_fill_bg(s_snow_canvas, lv_color_black(), LV_OPA_COVER);
    lv_obj_add_flag(s_snow_canvas, LV_OBJ_FLAG_HIDDEN);

    s_fire_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_fire_canvas, s_fire_buf, SCR_W, FIRE_BAND_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(s_fire_canvas, LV_ALIGN_TOP_LEFT, 0, FIRE_BAND_Y);
    lv_canvas_fill_bg(s_fire_canvas, lv_color_black(), LV_OPA_COVER);
    lv_obj_add_flag(s_fire_canvas, LV_OBJ_FLAG_HIDDEN);

    /* ---- 文字。画布先建，标签后建，这样标签压在动画之上 ---- */
    /*
     * 日期、时钟、温湿度三行放进竖排 flex 容器，容器按内容收缩后整体居中。
     * 温湿度那行文案宽度会变（"25.3°C 60%" / "-12.3°C 100%" / "传感器离线"），
     * 换文案不用重新算坐标。
     */
    s_content = lv_obj_create(scr);
    lv_obj_remove_style_all(s_content);               /* 去掉默认底色、边框、内边距 */
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_content, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_content, ROW_GAP, LV_PART_MAIN);

    s_date_label = lv_label_create(s_content);
    lv_obj_set_style_text_font(s_date_label, &font_sc_40, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_date_label, lv_color_hex(0x59C7F0), LV_PART_MAIN);
    lv_label_set_text(s_date_label, "");

    /* 字模里只有数字和冒号，占位符也只能用这些字符 */
    s_time_label = lv_label_create(s_content);
    lv_obj_set_style_text_font(s_time_label, &font_seg_96, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(s_time_label, CLOCK_LETTER_SPACE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_label_set_text(s_time_label, "00:00:00");

    s_env_label = lv_label_create(s_content);
    lv_obj_set_style_text_font(s_env_label, &font_sc_40, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_env_label, lv_color_hex(0xF0A030), LV_PART_MAIN);
    lv_label_set_text(s_env_label, "传感器离线");

    apply_layout(s_anim_mode);

    s_status_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_status_label, &font_sc_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x808A94), LV_PART_MAIN);
    lv_label_set_text(s_status_label, "连接中");
    lv_obj_align(s_status_label, LV_ALIGN_TOP_RIGHT, -12, 8);

    /*
     * ---- 亮度指示条 ----
     * 左侧一根竖条，没有面板底和文字。lv_bar 的方向由宽高比决定：高 > 宽
     * 就是竖向，填充从下往上长，和"往上滑变亮"的手势方向一致。
     * 高 160 居中落在 80..240，上不碰雪花带（0..44）、下不碰火焰带（264..）。
     */
    s_bright_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bright_bar, BRIGHT_BAR_W, BRIGHT_BAR_H);
    lv_obj_align(s_bright_bar, LV_ALIGN_LEFT_MID, BRIGHT_BAR_X, 0);
    lv_bar_set_range(s_bright_bar, 0, 100);
    lv_obj_set_style_radius(s_bright_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bright_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bright_bar, lv_color_hex(0x39424D), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bright_bar, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bright_bar, lv_color_hex(0xF8D848), LV_PART_INDICATOR);
    lv_obj_add_flag(s_bright_bar, LV_OBJ_FLAG_HIDDEN);

    /* ---- 定时器 ---- */
    lv_timer_create(clock_timer_cb, CLOCK_PERIOD_MS, NULL);
    lv_timer_create(anim_timer_cb, ANIM_PERIOD_MS, NULL);

    s_bright_timer = lv_timer_create(brightness_hide_cb, BRIGHTNESS_HIDE_MS, NULL);
    lv_timer_set_repeat_count(s_bright_timer, -1);
    lv_timer_pause(s_bright_timer);

    display_unlock();
    ESP_LOGI(TAG, "界面就绪");
}
