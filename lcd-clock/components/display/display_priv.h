/* display 组件内部用：touch.c 靠它把触摸芯片的物理坐标换算成当前方向下的 LVGL 坐标 */
#pragma once

#include <stdint.h>

void display_map_touch(uint16_t tx, uint16_t ty, uint16_t *x, uint16_t *y);
