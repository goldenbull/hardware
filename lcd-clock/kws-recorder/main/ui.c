#include "ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "fonts_sc.h"
#include "lvgl.h"
#include "mic.h"
#include "recorder.h"

#define UI_REFRESH_MS     100

/* 电平条量程：-60dBFS ~ 0dBFS */
#define METER_FLOOR_DB    (-60.0f)
/* 峰值保持后的回落速度，每次刷新最多掉这么多 dB，避免条子乱跳 */
#define METER_DECAY_DB    3.0f
/* 正常说话峰值落在 -20 ~ -6dB 最合适；超过 -3dB 标红提醒离远点或小声点 */
#define METER_WARN_DB     (-6.0f)
#define METER_HOT_DB      (-3.0f)

#define COLOR_BG          lv_color_hex(0x101418)
#define COLOR_PANEL       lv_color_hex(0x1E252C)
#define COLOR_TEXT        lv_color_hex(0xE8EAED)
#define COLOR_DIM         lv_color_hex(0x8A9199)
#define COLOR_REC         lv_color_hex(0xE5484D)
#define COLOR_OK          lv_color_hex(0x30A46C)
#define COLOR_WARN        lv_color_hex(0xF5A623)
#define COLOR_BTN_OPEN    lv_color_hex(0x2F6FDE)
#define COLOR_BTN_CLOSE   lv_color_hex(0x7C4DDB)
#define COLOR_BTN_STOP    lv_color_hex(0xC23B3F)

static lv_obj_t *s_storage_label;
static lv_obj_t *s_prompt_panel;
static lv_obj_t *s_prompt_top;
static lv_obj_t *s_prompt_word;
static lv_obj_t *s_time_label;
static lv_obj_t *s_file_label;
static lv_obj_t *s_quality_label;
static lv_obj_t *s_meter_bar;
static lv_obj_t *s_meter_label;
static lv_obj_t *s_btn_start[KEYWORD_COUNT];
static lv_obj_t *s_btn_stop;

static float s_meter_db = METER_FLOOR_DB;

static void on_start_clicked(lv_event_t *e)
{
    recorder_start((keyword_t)(intptr_t)lv_event_get_user_data(e));
}

static void on_stop_clicked(lv_event_t *e)
{
    recorder_stop();
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *title, const char *subtitle, lv_color_t color,
                             lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3A4048), LV_STATE_DISABLED);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(btn, 2, 0);

    lv_obj_t *t = make_label(btn, &font_sc_24, lv_color_white());
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, COLOR_DIM, LV_STATE_DISABLED);

    if (subtitle) {
        lv_obj_t *s = make_label(btn, &font_sc_16, lv_color_white());
        lv_label_set_text(s, subtitle);
        lv_obj_set_style_text_opa(s, LV_OPA_80, 0);
    }
    return btn;
}

/*
 * 下面几个 set_* 都是"值变了才动"。refresh_cb 每 100ms 跑一次，LVGL 8 的
 * lv_label_set_text / lv_obj_set_style_* 不管值变没变都会让对象重绘，
 * 不拦一下的话屏幕会一直在刷，白白占着和 TF 卡共用的 SPI 总线（HTTP 模式下也白占 CPU）。
 */
static void set_text(lv_obj_t *label, const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (strcmp(lv_label_get_text(label), buf) != 0) {
        lv_label_set_text(label, buf);
    }
}

static void set_text_color(lv_obj_t *obj, lv_color_t c)
{
    if (lv_obj_get_style_text_color(obj, LV_PART_MAIN).full != c.full) {
        lv_obj_set_style_text_color(obj, c, 0);
    }
}

static void set_border_color(lv_obj_t *obj, lv_color_t c)
{
    if (lv_obj_get_style_border_color(obj, LV_PART_MAIN).full != c.full) {
        lv_obj_set_style_border_color(obj, c, 0);
    }
}

static void set_enabled(lv_obj_t *obj, bool enabled)
{
    if (enabled == !lv_obj_has_state(obj, LV_STATE_DISABLED)) {
        return;
    }
    if (enabled) {
        lv_obj_clear_state(obj, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
    }
}

static void update_meter(uint16_t peak)
{
    float db = (peak > 0) ? 20.0f * log10f((float)peak / 32767.0f) : METER_FLOOR_DB;
    if (db < METER_FLOOR_DB) {
        db = METER_FLOOR_DB;
    }
    /* 上升立即跟上，下降慢慢回落 */
    if (db < s_meter_db - METER_DECAY_DB) {
        db = s_meter_db - METER_DECAY_DB;
    }
    s_meter_db = db;

    const int value = (int)((db - METER_FLOOR_DB) * 100.0f / -METER_FLOOR_DB);
    lv_bar_set_value(s_meter_bar, value, LV_ANIM_OFF);

    const lv_color_t c = (db >= METER_HOT_DB) ? COLOR_REC : (db >= METER_WARN_DB) ? COLOR_WARN : COLOR_OK;
    if (lv_obj_get_style_bg_color(s_meter_bar, LV_PART_INDICATOR).full != c.full) {
        lv_obj_set_style_bg_color(s_meter_bar, c, LV_PART_INDICATOR);
    }
    set_text(s_meter_label, "%d dB", (int)lroundf(db));
}

static void refresh_cb(lv_timer_t *t)
{
    rec_status_t st;
    recorder_get_status(&st);

    update_meter(st.peak);

    set_text(s_storage_label, "%s", st.storage);
    set_text_color(s_storage_label, st.storage_ok ? COLOR_DIM : COLOR_WARN);

    const bool busy = st.state != REC_IDLE;
    for (int i = 0; i < KEYWORD_COUNT; i++) {
        set_enabled(s_btn_start[i], !busy);
    }
    set_enabled(s_btn_stop, st.state == REC_RECORDING);

    switch (st.state) {
    case REC_RECORDING:
        set_text(s_prompt_top, "请说");
        set_text(s_prompt_word, "%s", recorder_keyword_text(st.keyword));
        set_text_color(s_prompt_word, lv_color_white());
        set_border_color(s_prompt_panel, COLOR_REC);
        break;
    case REC_STARTING:
        set_text(s_prompt_top, "");
        set_text(s_prompt_word, "准备中");
        set_text_color(s_prompt_word, COLOR_DIM);
        set_border_color(s_prompt_panel, COLOR_PANEL);
        break;
    case REC_STOPPING:
        set_text(s_prompt_top, "");
        set_text(s_prompt_word, "保存中");
        set_text_color(s_prompt_word, COLOR_DIM);
        set_border_color(s_prompt_panel, COLOR_PANEL);
        break;
    case REC_IDLE:
    default:
        set_text(s_prompt_top, "");
        set_text(s_prompt_word, "空闲");
        set_text_color(s_prompt_word, COLOR_DIM);
        set_border_color(s_prompt_panel, COLOR_PANEL);
        break;
    }

    const uint32_t secs = st.captured_samples / MIC_SAMPLE_RATE;
    set_text(s_time_label, "%02lu:%02lu", (unsigned long)(secs / 60), (unsigned long)(secs % 60));
    set_text_color(s_time_label, st.state == REC_RECORDING ? COLOR_REC : COLOR_TEXT);

    if (st.error) {
        set_text(s_file_label, "%s", st.error);
        set_text_color(s_file_label, COLOR_WARN);
    } else if (st.file[0]) {
        set_text(s_file_label, "%s%s", st.file, st.state == REC_IDLE ? "  已保存" : "");
        set_text_color(s_file_label, COLOR_TEXT);
    } else {
        set_text(s_file_label, "按下方按钮开始录音");
        set_text_color(s_file_label, COLOR_DIM);
    }

    /* 断网 / 写卡慢的时候录下来的还没落盘，积压超过 2 秒才显示，免得正常情况下一直在跳 */
    const uint32_t backlog_s = (st.captured_samples - st.samples) / MIC_SAMPLE_RATE;
    if (st.state != REC_IDLE && backlog_s >= 2) {
        set_text(s_quality_label, "削顶 %lu   丢帧 %lu ms   积压 %lu 秒",
                 (unsigned long)st.clipped_samples,
                 (unsigned long)(st.dropped_samples * 1000 / MIC_SAMPLE_RATE),
                 (unsigned long)backlog_s);
    } else {
        set_text(s_quality_label, "削顶 %lu   丢帧 %lu ms",
                 (unsigned long)st.clipped_samples,
                 (unsigned long)(st.dropped_samples * 1000 / MIC_SAMPLE_RATE));
    }
    set_text_color(s_quality_label, (st.clipped_samples || st.dropped_samples || backlog_s >= 2) ? COLOR_WARN : COLOR_DIM);
}

void ui_init(void)
{
    display_lock(-1);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 标题行 ---- */
    lv_obj_t *title = make_label(scr, &font_sc_24, COLOR_TEXT);
    lv_label_set_text(title, "关键词录音");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);

    s_storage_label = make_label(scr, &font_sc_16, COLOR_DIM);
    lv_obj_align(s_storage_label, LV_ALIGN_TOP_RIGHT, -12, 15);

    /* ---- 提示卡片 ---- */
    s_prompt_panel = lv_obj_create(scr);
    lv_obj_set_size(s_prompt_panel, 296, 150);
    lv_obj_align(s_prompt_panel, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_set_style_bg_color(s_prompt_panel, COLOR_PANEL, 0);
    lv_obj_set_style_border_width(s_prompt_panel, 3, 0);
    lv_obj_set_style_border_color(s_prompt_panel, COLOR_PANEL, 0);
    lv_obj_set_style_radius(s_prompt_panel, 12, 0);
    lv_obj_set_style_pad_all(s_prompt_panel, 8, 0);
    lv_obj_clear_flag(s_prompt_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_prompt_top = make_label(s_prompt_panel, &font_sc_16, COLOR_DIM);
    lv_obj_align(s_prompt_top, LV_ALIGN_TOP_MID, 0, 0);

    s_prompt_word = make_label(s_prompt_panel, &font_sc_40, COLOR_DIM);
    lv_obj_align(s_prompt_word, LV_ALIGN_CENTER, 0, -8);

    lv_obj_t *tips = make_label(s_prompt_panel, &font_sc_16, COLOR_DIM);
    lv_label_set_text(tips, "每说一次停 2~3 秒\n换着用大小声、快慢、不同语调");
    lv_obj_set_style_text_align(tips, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(tips, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* ---- 时长 / 文件 / 质量 ---- */
    s_time_label = make_label(scr, &font_sc_40, COLOR_TEXT);
    lv_obj_align(s_time_label, LV_ALIGN_TOP_MID, 0, 204);

    s_file_label = make_label(scr, &font_sc_16, COLOR_DIM);
    lv_obj_align(s_file_label, LV_ALIGN_TOP_MID, 0, 254);

    s_quality_label = make_label(scr, &font_sc_16, COLOR_DIM);
    lv_obj_align(s_quality_label, LV_ALIGN_TOP_MID, 0, 278);

    /* ---- 电平条 ---- */
    s_meter_bar = lv_bar_create(scr);
    lv_obj_set_size(s_meter_bar, 220, 14);
    lv_obj_align(s_meter_bar, LV_ALIGN_TOP_LEFT, 12, 310);
    lv_bar_set_range(s_meter_bar, 0, 100);
    lv_obj_set_style_bg_color(s_meter_bar, COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_anim_time(s_meter_bar, 0, 0);

    s_meter_label = make_label(scr, &font_sc_16, COLOR_DIM);
    lv_obj_align(s_meter_label, LV_ALIGN_TOP_RIGHT, -12, 307);

    /* ---- 按钮 ---- */
    static const char *start_titles[KEYWORD_COUNT] = {"开始录音1", "开始录音2"};
    for (int i = 0; i < KEYWORD_COUNT; i++) {
        s_btn_start[i] = make_button(scr, start_titles[i], recorder_keyword_text((keyword_t)i),
                                     i == KEYWORD_OPEN ? COLOR_BTN_OPEN : COLOR_BTN_CLOSE, 142, 64);
        lv_obj_align(s_btn_start[i], i == 0 ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_RIGHT, i == 0 ? 12 : -12, 340);
        lv_obj_add_event_cb(s_btn_start[i], on_start_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    s_btn_stop = make_button(scr, "结束录音", NULL, COLOR_BTN_STOP, 296, 56);
    lv_obj_align(s_btn_stop, LV_ALIGN_TOP_MID, 0, 414);
    lv_obj_add_event_cb(s_btn_stop, on_stop_clicked, LV_EVENT_CLICKED, NULL);

    refresh_cb(NULL);
    lv_timer_create(refresh_cb, UI_REFRESH_MS, NULL);

    display_unlock();
}
