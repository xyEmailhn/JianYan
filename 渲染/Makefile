# 简言 · 简渲（渲染 + 建模）构建脚本
#
#   make            速度最快（动态链接）
#   make 最小       体积最小且完全自包含（musl 静态链接）
#   make 静态       静态链接的速度版
#   make 调试       带符号、不优化
#   make 示例       四张示例图全部渲一遍
#   make 预览       低采样快速看构图
#   make 模型       把小岛导出成 obj + glb
#   make 测         跑一遍性能基准
#   make 干净       删产物

CC = gcc

ZIG = $(shell command -v zig 2>/dev/null || ls /usr/local/lib/python3*/dist-packages/ziglang/zig 2>/dev/null | head -1)

快 = -O3 -march=native -ffast-math -fno-math-errno -flto -fno-plt \
     -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident \
     -ffunction-sections -fdata-sections -Wl,--gc-sections

.PHONY: all 最小 静态 调试 示例 预览 模型 测 干净

all: 简渲

简渲: 简渲.c 着色.c 引擎.c 场景.c 图像.c 建模.c 数学.h
	$(CC) $(快) -s -o $@ 简渲.c -lm -lpthread

最小: 简渲.c
	@test -n "$(ZIG)" || (echo "需要 zig：pip3 install --break-system-packages ziglang"; false)
	$(ZIG) cc -target x86_64-linux-musl -Oz -s -o 简渲_最小 简渲.c -lm -lpthread

静态: 简渲.c
	@test -n "$(ZIG)" || (echo "需要 zig：pip3 install --break-system-packages ziglang"; false)
	$(ZIG) cc -target x86_64-linux-musl -O3 -s -o 简渲_静态 简渲.c -lm -lpthread

调试: 简渲.c
	$(CC) -O0 -g -fsanitize=address,undefined -o 简渲_调试 简渲.c -lm -lpthread

示例: 简渲
	@mkdir -p 出图
	./简渲 示例/静物.scene -o 出图/静物.png
	./简渲 示例/小岛.scene -o 出图/小岛.png
	./简渲 示例/材质.scene -o 出图/材质.png
	./简渲 示例/几何.scene -o 出图/几何.png

预览: 简渲
	@mkdir -p 出图
	./简渲 示例/静物.scene -预览 -o 出图/预览_静物.png
	./简渲 示例/小岛.scene -预览 -o 出图/预览_小岛.png

模型: 简渲
	@mkdir -p 出图
	./简渲 示例/小岛.scene -模型 出图/小岛

测: 简渲
	@echo "== 900×560 / 128 采样 =="
	./简渲 示例/静物.scene -o /tmp/基准.png

干净:
	rm -f 简渲 简渲_最小 简渲_静态 简渲_调试
	rm -rf 出图
