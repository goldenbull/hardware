#include "debug_panel.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "display.h"
#include "driver/temperature_sensor.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "fonts_sc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imu.h"
#include "lvgl.h"
#include "mic.h"
#include "net_time.h"
#include "sdkconfig.h"
#include "voice.h"

static const char *TAG = "debug";

#define SCR_W              480
#define SCR_H              320
#define COL_W              (SCR_W / 2)
#define PAD_X              12

/*
 * 检查周期短、刷新周期长：屏一关就要尽快切回主屏（下次开屏不能先闪一下面板），
 * 而面板内容一秒变一次就够了。
 */
#define CHECK_PERIOD_MS    100
#define REFRESH_PERIOD_MS  1000

#define SLIDE_ANIM_MS      200

/*
 * 更纱黑体 Fixed 是等宽字体，汉字正好占两个 ASCII 宽。每行"名称"补空格到这么多列，
 * 两栏的值就能竖着对齐。16 号字一列 8px，一栏 240px 放 30 列，名称 10 列、值 20 列。
 */
#define KEY_COLS           10

#define LINE_BUF           64
#define TEXT_BUF           1024

static lv_obj_t   *s_main_scr;
static lv_obj_t   *s_scr;
static lv_obj_t   *s_left;
static lv_obj_t   *s_right;
static bool        s_visible;
static int64_t     s_last_refresh_us;

static temperature_sensor_handle_t s_tsens;

static portMUX_TYPE   s_sensor_lock = portMUX_INITIALIZER_UNLOCKED;
static debug_sensor_t s_sensor;

/* ------------------------------------------------------------------ */
/* 文本拼装                                                            */
/* ------------------------------------------------------------------ */

/* UTF-8 字符串在等宽字体下占几列：三字节（汉字、全角标点）算 2，其余算 1 */
static int text_cols(const char *s)
{
    int cols = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if (*p < 0x80) {
            cols += 1;
            p += 1;
        } else if (*p < 0xE0) {
            cols += 1;      /* 两字节，如 ° */
            p += 2;
        } else if (*p < 0xF0) {
            cols += 2;
            p += 3;
        } else {
            cols += 2;
            p += 4;
        }
    }
    return cols;
}

/* 往 text 末尾追加一行 "名称<补空格>值\n" */
static void add_row(char *text, const char *key, const char *fmt, ...)
{
    char line[LINE_BUF];
    int n = snprintf(line, sizeof(line), "%s", key);
    for (int c = text_cols(key); c < KEY_COLS && n < (int)sizeof(line) - 1; c++) {
        line[n++] = ' ';
    }
    line[n] = '\0';

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);

    const size_t len = strlen(text);
    snprintf(text + len, TEXT_BUF - len, "%s%s", len ? "\n" : "", line);
}

static void format_bytes(char *buf, size_t size, size_t bytes)
{
    if (bytes >= 1024 * 1024) {
        snprintf(buf, size, "%.1fM", bytes / (1024.0 * 1024.0));
    } else {
        snprintf(buf, size, "%uK", (unsigned)(bytes / 1024));
    }
}

static const char *reset_reason_text(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "上电";
    case ESP_RST_EXT:       return "外部复位";
    case ESP_RST_SW:        return "软件重启";
    case ESP_RST_PANIC:     return "崩溃重启";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "看门狗";
    case ESP_RST_DEEPSLEEP: return "深睡唤醒";
    case ESP_RST_BROWNOUT:  return "欠压";
    case ESP_RST_USB:       return "USB 复位";
    case ESP_RST_JTAG:      return "JTAG 复位";
    default:                return "未知";
    }
}

static const char *net_state_text(net_state_t s)
{
    switch (s) {
    case NET_STATE_CONNECTING: return "连接中";
    case NET_STATE_SYNCING:    return "校时中";
    case NET_STATE_READY:      return "已校时";
    case NET_STATE_OFFLINE:    return "离线";
    default:                   return "未知";
    }
}

/* ------------------------------------------------------------------ */
/* 两栏内容                                                            */
/* ------------------------------------------------------------------ */

static void build_system(char *text)
{
    text[0] = '\0';

    /* esp_timer 从这次启动开始计时；上一次是怎么重启的看"启动原因" */
    const uint64_t up = esp_timer_get_time() / 1000000ULL;
    add_row(text, "运行时间", "%lu天 %02lu:%02lu:%02lu",
            (unsigned long)(up / 86400), (unsigned long)(up / 3600 % 24),
            (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));
    add_row(text, "启动原因", "%s", reset_reason_text(esp_reset_reason()));

    const esp_app_desc_t *app = esp_app_get_description();
    add_row(text, "编译时间", "%.6s %.5s", app->date, app->time);    /* "Oct 15" "10:22" */
    add_row(text, "IDF", "%s", app->idf_ver);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    add_row(text, "芯片", "%s r%d.%d", CONFIG_IDF_TARGET, chip.revision / 100, chip.revision % 100);
    add_row(text, "主频", "%dMHz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);

    float chip_temp = 0;
    if (s_tsens && temperature_sensor_get_celsius(s_tsens, &chip_temp) == ESP_OK) {
        add_row(text, "芯片温度", "%.1f°C", chip_temp);
    } else {
        add_row(text, "芯片温度", "不可用");
    }

    char a[12], b[12];
    format_bytes(a, sizeof(a), heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    format_bytes(b, sizeof(b), heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    add_row(text, "内部内存", "%s 最低 %s", a, b);

    format_bytes(a, sizeof(a), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    format_bytes(b, sizeof(b), heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    add_row(text, "PSRAM", "%s / %s", a, b);

    add_row(text, "任务数", "%u", (unsigned)uxTaskGetNumberOfTasks());

    add_row(text, "屏幕", "亮度 %u%% %s", display_get_brightness(),
            display_is_flipped() ? "已翻转" : "未翻转");
}

static void build_peripherals(char *text)
{
    text[0] = '\0';

    /* ---- 网络 ---- */
    add_row(text, "网络", "%s", net_state_text(net_time_get_state()));

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        add_row(text, "SSID", "%.20s", (const char *)ap.ssid);
        add_row(text, "信号", "%d dBm 信道 %d", ap.rssi, ap.primary);
    } else {
        add_row(text, "SSID", "未连接（%.12s）", CONFIG_APP_WIFI_SSID);
        add_row(text, "信号", "-");
    }

    esp_netif_ip_info_t ip = {0};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        add_row(text, "IP", IPSTR, IP2STR(&ip.ip));
    } else {
        add_row(text, "IP", "-");
    }

    net_stats_t ns;
    net_time_get_stats(&ns);
    if (ns.last_sync) {
        struct tm t;
        localtime_r(&ns.last_sync, &t);
        add_row(text, "校时", "%lu次 上次%02d:%02d:%02d", (unsigned long)ns.sync_count,
                t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        add_row(text, "校时", "未校时");
    }
    if (ns.disconnect_count) {
        add_row(text, "断线", "%lu次 原因%d", (unsigned long)ns.disconnect_count, ns.last_disconnect_reason);
    } else {
        add_row(text, "断线", "0次");
    }

    /* ---- 传感器 ---- */
    debug_sensor_t sn;
    taskENTER_CRITICAL(&s_sensor_lock);
    sn = s_sensor;
    taskEXIT_CRITICAL(&s_sensor_lock);

    if (sn.present) {
        add_row(text, "SHT45", "%.1f°C %.0f%%", sn.temperature_c, sn.humidity_rh);
    } else {
        add_row(text, "SHT45", "未检测到");
    }
    add_row(text, "  探测/读", "%lu/%lu 失败%lu", (unsigned long)sn.probe_count,
            (unsigned long)sn.read_ok, (unsigned long)sn.read_fail);

    /* ---- IMU ---- */
    float ax, ay, az;
    if (imu_is_ready() && imu_read_accel(&ax, &ay, &az) == ESP_OK) {
        add_row(text, "IMU(g)", "%+.2f %+.2f %+.2f", ax, ay, az);
    } else {
        add_row(text, "IMU(g)", "不可用");
    }

    /* ---- 麦克风 ---- */
    if (voice_is_running()) {
        const uint16_t peak = voice_take_peak();
        const int db = peak ? (int)lroundf(20.0f * log10f(peak / 32767.0f)) : -96;
        add_row(text, "麦克风", "峰值%ddB 增益%d", db, MIC_GAIN_DB);
#if CONFIG_APP_VOICE_MODE_LEVEL
        /* 最近 1 秒平滑音量的最大值，对着 menuconfig 里的阈值调；触发次数包括被间隔挡掉的 */
#if CONFIG_APP_SOUND_TRIGGER_ENABLE
        add_row(text, "  声控", "%ddB/%d 触发%lu", voice_take_level_max_db(), CONFIG_APP_SOUND_THRESHOLD_DB,
                (unsigned long)voice_trigger_count());
#else
        add_row(text, "  声控", "%ddB/%d 未启用", voice_take_level_max_db(), CONFIG_APP_SOUND_THRESHOLD_DB);
#endif
#else
        add_row(text, "  声控", "关键词（未实现）");
#endif
    } else {
        add_row(text, "麦克风", "未启用");
    }
}

static void refresh(void)
{
    static char text[TEXT_BUF];

    build_system(text);
    lv_label_set_text(s_left, text);

    build_peripherals(text);
    lv_label_set_text(s_right, text);

    s_last_refresh_us = esp_timer_get_time();
}

/* ------------------------------------------------------------------ */
/* 显示 / 隐藏                                                         */
/* ------------------------------------------------------------------ */

/* 调用方已经拿着 LVGL 锁 */
static void load_main(bool animate)
{
    s_visible = false;
    if (animate) {
        lv_scr_load_anim(s_main_scr, LV_SCR_LOAD_ANIM_MOVE_RIGHT, SLIDE_ANIM_MS, 0, false);
    } else {
        lv_scr_load(s_main_scr);
    }
}

static void check_timer_cb(lv_timer_t *t)
{
    if (!s_visible) {
        return;
    }
    /* 关屏（单击或语音）之后直接切回主屏，不做动画，下次开屏就是时钟 */
    if (!display_is_on()) {
        load_main(false);
        ESP_LOGI(TAG, "屏幕已关，调试面板切回主屏");
        return;
    }
    if (esp_timer_get_time() - s_last_refresh_us >= REFRESH_PERIOD_MS * 1000LL) {
        refresh();
    }
}

void debug_panel_show(void)
{
    if (!display_lock(200)) {
        return;
    }
    if (!s_visible && display_is_on()) {
        s_visible = true;
        refresh();
        lv_scr_load_anim(s_scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, SLIDE_ANIM_MS, 0, false);
    }
    display_unlock();
}

void debug_panel_hide(void)
{
    if (!display_lock(200)) {
        return;
    }
    if (s_visible) {
        load_main(true);
    }
    display_unlock();
}

void debug_panel_report_sensor(const debug_sensor_t *sensor)
{
    taskENTER_CRITICAL(&s_sensor_lock);
    s_sensor = *sensor;
    taskEXIT_CRITICAL(&s_sensor_lock);
}

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */

static lv_obj_t *make_column(lv_obj_t *parent, lv_coord_t x, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &font_sc_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(l, 4, LV_PART_MAIN);   /* 左栏 11 行，行距再大就超出 320 了 */
    lv_obj_set_width(l, COL_W - PAD_X);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, "");
    lv_obj_set_pos(l, x, 40);
    return l;
}

void debug_panel_init(void)
{
    /* 芯片内部温度传感器，量程选 -10~80°C 这一档，精度最好 */
    temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tcfg, &s_tsens) != ESP_OK || temperature_sensor_enable(s_tsens) != ESP_OK) {
        ESP_LOGW(TAG, "芯片温度传感器不可用");
        s_tsens = NULL;
    }

    display_lock(-1);

    s_main_scr = lv_scr_act();

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x0B0F14), LV_PART_MAIN);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_obj_set_style_text_font(title, &font_sc_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0x59C7F0), LV_PART_MAIN);
    lv_label_set_text(title, "调试信息");
    lv_obj_set_pos(title, PAD_X, 10);

    lv_obj_t *hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(hint, &font_sc_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x5A6470), LV_PART_MAIN);
    lv_label_set_text(hint, "向右滑动返回");
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -PAD_X, 10);

    lv_obj_t *line = lv_obj_create(s_scr);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, SCR_W - 2 * PAD_X, 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x2A323C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_pos(line, PAD_X, 32);

    s_left = make_column(s_scr, PAD_X, lv_color_hex(0xD0D6DC));
    s_right = make_column(s_scr, COL_W + 4, lv_color_hex(0xD0D6DC));

    lv_timer_create(check_timer_cb, CHECK_PERIOD_MS, NULL);

    display_unlock();
}
