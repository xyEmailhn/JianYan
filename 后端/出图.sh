#!/bin/bash
# 一键出图：几份 .jy 源码 → 一批成品。
#
#   bash 出图.sh
#
# 依赖：gcc（编 简编译）、ffmpeg（动画）、cairosvg（SVG 转 PNG，可选）
#
# 注意 v0.7 之后的变化：
#   静帧、封面、滤镜全部由 简言 自己写成 PNG，不再走 PPM → convert。
#   剩下的外部依赖只有两个：ffmpeg（做视频）、cairosvg（把 SVG 转成位图预览）。
#
# 产出（都在 出图/ 下）：
#   光线.png        640×400    光线追踪，直出
#   滤镜_模糊.png   640×400    高斯模糊
#   滤镜_描边.png   640×400    Sobel 边缘
#   滤镜_调色.png   640×400    色调分离
#   封面.png        1200×750   纯像素 + 中文字体画的封面
#   绘图.svg        1200×750   矢量海报，直接就是 SVG
#   绘图.png        1200×750   上面那张的位图版，用来嵌网页
#   动画.gif        560×350    3 秒转台，60 帧
#   动画.mp4        960×600    H.264
#   主题.wav        8 秒       五声音阶循环，逐帧合成
#   声纹.png        1600×600   47 秒人声的波形

set -e
cd "$(dirname "$0")"

[ -x ./简编译 ] || make
[ -f ../第三方/lib简言图.a ] || make -C ../第三方

mkdir -p 出图
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "① 光线静帧（直出 PNG）"
./简编译 示例/光线.jy -o "$TMP/光线"
"$TMP/光线"

echo "② 滤镜链（读图 → 处理 → 存图）"
./简编译 示例/滤镜.jy -o "$TMP/滤镜"
"$TMP/滤镜"

echo "③ 封面（纯像素 + 中文字体）"
./简编译 示例/封面.jy -o "$TMP/封面"
"$TMP/封面"

echo "④ 矢量海报"
./简编译 示例/绘图.jy -o "$TMP/绘图"
"$TMP/绘图" > 出图/绘图.svg
python3 - "$PWD/出图/绘图.svg" "$PWD/出图/绘图.png" <<'PY'
import sys, cairosvg
cairosvg.svg2png(url=sys.argv[1], write_to=sys.argv[2], output_width=1200, output_height=750)
PY

echo "⑤ 转台动画（60 帧，约 25 秒）"
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

echo "⑥ 合成一段声音（正弦 + 包络 + 回声）"
./简编译 示例/声音.jy -o "$TMP/声音"
"$TMP/声音"

# 声纹要读 出图/大肥鱼.mp3。这个文件是语音合成的产物，
# 不进仓库也没有关系——没有它就跳过，不报错。
if [ -f 出图/大肥鱼.mp3 ]; then
    echo "⑦ 声纹（读 MP3 → 逐列 min/max → PNG）"
    ./简编译 示例/声画.jy -o "$TMP/声画"
    "$TMP/声画"
else
    echo "⑦ 跳过：没有 出图/大肥鱼.mp3"
fi

echo
ls -lh 出图/
