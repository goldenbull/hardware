/*
 * 调试面板：时钟屏上从右往左划进来，从左往右划回去。
 *
 * 面板是一块独立的 LVGL screen，不是盖在时钟上的浮层——显示面板时时钟那边的
 * 动画和秒针刷新都落在非活动 screen 上，LVGL 不会去重绘它们，SPI 总线是空的。
 *
 * 单击照常关屏；屏一关面板就自动切回时钟，下次开屏看到的总是时钟。
 * 内容每秒刷新一次，只在面板可见且屏亮着时刷新。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 传感器任务每轮上报一次，面板上显示 */
typedef struct {
    bool     present;
    float    temperature_c;
    float    humidity_rh;
    uint32_t probe_count;   /* 探测次数（没插传感器时这个数会一直涨） */
    uint32_t read_ok;
    uint32_t read_fail;
} debug_sensor_t;

/* 必须在 ui_init() 之后调用：把当时的活动 screen 记作"主屏" */
void debug_panel_init(void);

/* 这两个可以在任意任务里调，内部会拿 LVGL 锁 */
void debug_panel_show(void);
void debug_panel_hide(void);

void debug_panel_report_sensor(const debug_sensor_t *sensor);
