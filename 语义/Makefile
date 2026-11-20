# 简言 · 语义分析器 构建脚本
#
#   make          速度最快（动态链接）
#   make 最小      体积最小且完全自包含（musl 静态链接）
#   make 静态      静态链接的速度版
#   make 调试      带符号、不优化
#   make 跑        跑一遍正确示例
#   make 对答案    C 版和 Python 版各出一份报告，去掉行号后逐字节比对
#   make 测        造一份大源码，测吞吐

CC = gcc

ZIG = $(shell command -v zig 2>/dev/null || ls /usr/local/lib/python3*/dist-packages/ziglang/zig 2>/dev/null | head -1)

快 = -O3 -flto -fno-plt -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident

.PHONY: all 最小 静态 调试 跑 对答案 测 干净

all: 简语义

简语义: sema.c ../语法/parser.c ../词法/lexer.c
	$(CC) $(快) -s -o $@ sema.c

最小: sema.c
	@test -n "$(ZIG)" || (echo "需要 zig：pip3 install --break-system-packages ziglang"; false)
	$(ZIG) cc -target x86_64-linux-musl -Oz -s -o 简语义_最小 sema.c

静态: sema.c
	@test -n "$(ZIG)" || (echo "需要 zig：pip3 install --break-system-packages ziglang"; false)
	$(ZIG) cc -target x86_64-linux-musl -O3 -s -o 简语义_静态 sema.c

调试: sema.c
	$(CC) -O0 -g -o 简语义_调试 sema.c

跑: 简语义
	@./简语义 示例/正确.jy
	@echo "---"
	@./简语义 示例/错误.jy || true

# 去掉 "第 N 行 第 M 列：" 再比，因为 Python 那棵树不带位置信息
对答案: 简语义
	@sh 对答案.sh

测: 简语义
	@python3 -c "L=['结构 点 {','    x: 整数','    y: 整数','}','','函数 大函数() -> 整数 {','    私有 整数 累加 = 0'];L+=['    累加 = 累加 + %d %% 7'%(i%97) for i in range(20000)];L+=['    返回 累加','}'];open('/tmp/大源码.jy','w',encoding='utf-8').write('\n'.join(L))"
	@./简语义 -基准 /tmp/大源码.jy

干净:
	rm -f 简语义 简语义_最小 简语义_静态 简语义_调试
