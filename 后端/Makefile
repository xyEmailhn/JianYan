# 简言 · 后端 构建脚本
#
#   make          构建 简编译
#   make 最小      体积最小（musl 静态）
#   make 测试      跑全部测试用例
#   make 示例      编译并运行全部示例
#   make 出图      跑 出图.sh：静帧 + 海报 + 动画
#   make 干净      清掉产物

CC = gcc

ZIG = $(shell command -v zig 2>/dev/null || ls /usr/local/lib/python3*/dist-packages/ziglang/zig 2>/dev/null | head -1)

快 = -O3 -flto -fno-plt -fno-asynchronous-unwind-tables -fno-unwind-tables \
     -fno-ident -ffunction-sections -Wl,--gc-sections

.PHONY: all 最小 测试 示例 出图 图库 干净

all: 简编译 图库

简编译: 简编译.c 简汇编.c 简出.c ../中间/ir.c ../语义/sema.c ../语法/parser.c ../词法/lexer.c
	$(CC) $(快) -s -o $@ 简编译.c -lm

最小: 简编译.c
	@test -n "$(ZIG)" || (echo "需要 zig：pip3 install --break-system-packages ziglang"; false)
	$(ZIG) cc -target x86_64-linux-musl -Oz -s -o 简编译_最小 简编译.c -lm

测试: 简编译
	@bash 测试.sh

示例: 简编译
	@for f in 示例/*.jy; do \
	    echo "=== $$f ==="; \
	    ./简编译 "$$f" -o /tmp/简言示例 || exit 1; \
	    /tmp/简言示例 || true; \
	    echo; \
	done

出图: 简编译
	@bash 出图.sh

图库:
	@$(MAKE) -C ../第三方

干净:
	rm -f 简编译 简编译_最小 简编译_静态
	@$(MAKE) -C ../第三方 干净
