/*
 * 字模，由 lv_font_conv 生成，重新生成的命令见 tools/gen_fonts.sh。
 * 改界面文案时如果用到了新的汉字，记得把字加进那个脚本的 CJK 变量里重新跑一遍，
 * 否则新字会显示成方框。
 *
 *   font_sc_16   更纱黑体，ASCII + 汉字，状态提示用
 *   font_sc_40   更纱黑体，ASCII + ° + 星期 + 传感器离线，日期行 / 温湿度行
 *   font_seg_96  DSEG7 Classic Bold，只有数字和冒号，大号时钟
 */
#pragma once

#include "lvgl.h"

extern const lv_font_t font_sc_16;
extern const lv_font_t font_sc_40;
extern const lv_font_t font_seg_96;
