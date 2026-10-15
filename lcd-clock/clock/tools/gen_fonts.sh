#!/usr/bin/env bash
#
# 重新生成 main/fonts/ 下的字模。和 amoled-clock 用同一套字体：
#   更纱黑体 Fixed SC（等宽，SIL OFL）—— 日期、温湿度、状态提示
#   DSEG7 Classic Bold（七段数码管，SIL OFL）—— 大号时钟
#
# 需要 node（脚本会用 npx 拉 lv_font_conv）。
# 界面上新增汉字之后，把字补进下面的 CJK 再跑一次，否则新字显示成方框。
set -euo pipefail

FONT_DIR="$(cd "$(dirname "$0")/../../../字模提取V2.2" && pwd)"
TTF="${1:-$FONT_DIR/SarasaFixedSC-Regular.ttf}"
SEG_TTF="${2:-$FONT_DIR/DSEG7Classic-Bold.ttf}"
OUT="$(cd "$(dirname "$0")/../main/fonts" && pwd)"

# 时钟主屏上出现的汉字
CJK="周一二三四五六日温度湿传感器离线连接中校时亮"

# 调试面板的文案多、改得勤，直接从 debug_panel.c 的字符串字面量里提取非 ASCII 字符
DEBUG_CJK="$(python3 - "$(dirname "$0")/../main/debug_panel.c" <<'PY'
import re, sys
src = open(sys.argv[1], encoding="utf-8").read()
chars = set()
for lit in re.findall(r'"((?:[^"\\\n]|\\.)*)"', src):
    chars.update(c for c in lit if ord(c) > 0x7E and c != "\u00b0")
print("".join(sorted(chars)), end="")
PY
)"
CJK="$CJK$DEBUG_CJK"

# -r 0x20-0x7E  ASCII
# -r 0xB0       温度的 ° 号
COMMON=(--bpp 4 --no-compress --format lvgl --lv-include lvgl.h)

npx --yes lv_font_conv@1.5.3 --font "$TTF" --size 16 "${COMMON[@]}" \
    -r 0x20-0x7E -r 0xB0 --symbols "$CJK" -o "$OUT/font_sc_16.c"

# 横屏 480x320 比 amoled 的 536x240 窄一点、高很多，日期和温湿度不再挤在同一行，
# 各占一行，字号就能放大到 40："2026-10-11 周六" 约 300px
INFO_CJK="周一二三四五六日传感器离线"
npx --yes lv_font_conv@1.5.3 --font "$TTF" --size 40 "${COMMON[@]}" \
    -r 0x20-0x7E -r 0xB0 --symbols "$INFO_CJK" -o "$OUT/font_sc_40.c"

# 大号时钟只有 00:00:00。字号受宽度限制：amoled 上 108 号配 -6 字间距着墨宽 510px，
# 按比例 96 号配 -5 字间距约 455px，480 宽的屏左右各留十来像素
npx --yes lv_font_conv@1.5.3 --font "$SEG_TTF" --size 96 "${COMMON[@]}" \
    --symbols "0123456789:" -o "$OUT/font_seg_96.c"

echo "生成完成："
ls -l "$OUT"
