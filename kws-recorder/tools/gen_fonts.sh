#!/usr/bin/env bash
#
# 重新生成 main/fonts/ 下的中文字模。
#
# 需要 node（脚本会用 npx 拉 lv_font_conv）和 python3，以及一份中文 TTF。
# 默认用和 amoled-clock 同一份 更纱黑体 Fixed SC（SIL OFL 授权，可以随固件分发）。
#
# 界面用到的汉字直接从 main/ui.c、main/recorder.c 的字符串字面量里提取，
# 改了界面文案之后重跑一次就行，不用手动维护字表。
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/.."
TTF="${1:-$ROOT/../字模提取V2.2/SarasaFixedSC-Regular.ttf}"
OUT="$ROOT/main/fonts"

# 只取双引号字符串里的非 ASCII 字符，注释里的汉字不进字模
CJK="$(python3 - "$ROOT/main/ui.c" "$ROOT/main/recorder.c" <<'PY'
import re, sys
chars = set()
for path in sys.argv[1:]:
    src = open(path, encoding="utf-8").read()
    for lit in re.findall(r'"((?:[^"\\\n]|\\.)*)"', src):
        chars.update(c for c in lit if ord(c) > 0x7E)
print("".join(sorted(chars)), end="")
PY
)"
echo "界面汉字：$CJK"

COMMON=(--bpp 4 --no-compress --format lvgl --lv-include lvgl.h -r 0x20-0x7E --symbols "$CJK")

for size in 16 24 40; do
    npx --yes lv_font_conv@1.5.3 --font "$TTF" --size "$size" "${COMMON[@]}" -o "$OUT/font_sc_$size.c"
done

echo "生成完成："
ls -l "$OUT"
