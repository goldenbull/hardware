#!/usr/bin/env bash
#
# 重新生成 main/fonts/ 下的中文字模。
#
# 需要 node（脚本会用 npx 拉 lv_font_conv），以及一份中文 TTF。
# 默认用 repo 里的 更纱黑体 Fixed SC —— 等宽，SIL OFL 授权，可以随固件分发。
#
# 界面上新增汉字之后，把字补进下面的 CJK 再跑一次，否则新字显示成方框。
set -euo pipefail

TTF="${1:-../../字模提取V2.2/SarasaFixedSC-Regular.ttf}"
OUT="$(cd "$(dirname "$0")/../main/fonts" && pwd)"

# 界面上出现的全部汉字
CJK="周一二三四五六日温度湿传感器离线连接中校时亮"

# -r 0x20-0x7E  ASCII
# -r 0xB0       温度的 ° 号
COMMON=(--bpp 4 --no-compress --format lvgl --lv-include lvgl.h)

npx --yes lv_font_conv@1.5.3 --font "$TTF" --size 16 "${COMMON[@]}" \
    -r 0x20-0x7E -r 0xB0 --symbols "$CJK" -o "$OUT/font_sc_16.c"

npx --yes lv_font_conv@1.5.3 --font "$TTF" --size 28 "${COMMON[@]}" \
    -r 0x20-0x7E -r 0xB0 --symbols "$CJK" -o "$OUT/font_sc_28.c"

# 日期行只有 "2026-08-07  周五"，汉字只用得到星期，不必带全套 CJK
npx --yes lv_font_conv@1.5.3 --font "$TTF" --size 34 "${COMMON[@]}" \
    -r 0x20-0x7E --symbols "周一二三四五六日" -o "$OUT/font_sc_34.c"

# 大号时钟只有 00:00:00，不需要别的字符
npx --yes lv_font_conv@1.5.3 --font "$TTF" --size 96 "${COMMON[@]}" \
    --symbols "0123456789:" -o "$OUT/font_sc_96.c"

echo "生成完成："
ls -l "$OUT"
