#!/bin/bash
# 一键出图：三份 .jy 源码 → 三张成品。
#
#   bash 出图.sh
#
# 依赖：gcc（编 简编译）、ImageMagick（convert）、cairosvg（SVG 转 PNG）、ffmpeg
#
# 产出（都在 出图/ 下）：
#   光线.png   640×400   静帧，PPM → PNG
#   绘图.svg   1200×750  矢量海报，直接就是 SVG
#   绘图.png   1200×750  上面那张的位图版，用来嵌网页
#   动画.gif   560×350   3 秒转台，60 帧
#   动画.mp4   960×600   H.264，给不吃 GIF 的地方用

set -e
cd "$(dirname "$0")"

[ -x ./简编译 ] || make

mkdir -p 出图
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "① 光线静帧"
./简编译 示例/光线.jy -o "$TMP/光线"
"$TMP/光线" > "$TMP/光线.ppm"
convert "$TMP/光线.ppm" 出图/光线.png

echo "② 矢量海报"
./简编译 示例/绘图.jy -o "$TMP/绘图"
"$TMP/绘图" > 出图/绘图.svg
python3 - "$PWD/出图/绘图.svg" "$PWD/出图/绘图.png" <<'PY'
import sys, cairosvg
cairosvg.svg2png(url=sys.argv[1], write_to=sys.argv[2], output_width=1200, output_height=750)
PY

echo "③ 转台动画（60 帧，约 10 秒）"
./简编译 示例/动画.jy -o "$TMP/动画"
"$TMP/动画" > "$TMP/帧.txt"

# 拆帧：每帧前面有一行 "帧 N"，按它切
python3 - "$TMP/帧.txt" "$TMP" <<'PY'
import io, re, sys
s = io.open(sys.argv[1], encoding='utf-8', errors='replace').read()
块 = [p.strip() for p in re.split(r'^帧 \d+\s*$', s, flags=re.M) if p.strip()]
for i, p in enumerate(块):
    io.open('%s/f%03d.ppm' % (sys.argv[2], i), 'w', encoding='ascii').write(p + '\n')
print('   拆出 %d 帧' % len(块))
PY

ffmpeg -y -loglevel error -framerate 20 -i "$TMP/f%03d.ppm" \
    -vf "scale=560:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];[b][p]paletteuse=dither=sierra2_4a" \
    -loop 0 出图/动画.gif

ffmpeg -y -loglevel error -framerate 20 -i "$TMP/f%03d.ppm" \
    -vf "scale=960:-1:flags=lanczos" -c:v libopenh264 -b:v 2M -pix_fmt yuv420p 出图/动画.mp4

echo
ls -lh 出图/
