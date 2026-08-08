/* LVGL 界面：时钟 + 温湿度 + 冷热动画 + 亮度提示 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "net_time.h"

void ui_init(void);

/* 传感器读数更新，ok=false 表示传感器掉线 */
void ui_set_environment(bool ok, float temperature_c, float humidity_rh);

/* 网络/校时状态，显示在右上角 */
void ui_set_net_state(net_state_t state);

/* 上下滑动调亮度时，左侧的指示条 */
void ui_show_brightness(uint8_t brightness);
void ui_hide_brightness(void);
