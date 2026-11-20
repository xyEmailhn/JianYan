# 简言 · 词法分析器 构建脚本
#
# 四个目标，按需要选：
#   make          速度最快（动态链接，24 KB）
#   make 最小      体积最小且完全自包含（musl 静态链接，37 KB，拷到任何 Linux 都能跑）
#   make 静态      静态链接的速度版（39 KB）
#   make 调试      带符号、不优化，出问题用它
#
# 说明：最小/静态两个目标需要 zig（它自带 musl 运行库，一条命令就能出静态二进制）。
#       没装 zig 也不影响 `make`，只是静态版打不出来。

CC = gcc

# 找 zig：优先 PATH，其次 pip 装的 ziglang 包
ZIG = $(shell command -v zig 2>/dev/null || ls /usr/local/lib/python3*/dist-packages/ziglang/zig 2>/dev/null | head -1)

# 速度优先：开满优化、跨文件优化、去掉异常回滚表和编译器签名
快 = -O3 -flto -fno-plt -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident

.PHONY: all 最小 静态 调试 跑 测 干净

all: 简词法

简词法: lexer.c
	$(CC) $(快) -s -o $@ $<

# musl 静态链接：不依赖系统 glibc，体积只有 glibc 静态版的 1/20
最小: lexer.c
	@test -n "$(ZIG)" || (echo "需要 zig：pip3 install --break-system-packages ziglang"; false)
	$(ZIG) cc -target x86_64-linux-musl -Oz -s -o 简词法_最小 lexer.c

静态: lexer.c
	@test -n "$(ZIG)" || (echo "需要 zig：pip3 install --break-system-packages ziglang"; false)
	$(ZIG) cc -target x86_64-linux-musl -O3 -s -o 简词法_静态 lexer.c

调试: lexer.c
	$(CC) -O0 -g -o 简词法_调试 lexer.c

跑: 简词法
	./简词法 示例/词频.简

测: 简词法
	@python3 -c "s=open('示例/词频.简',encoding='utf-8').read();open('/tmp/大文件.简','w',encoding='utf-8').write(s*6000)"
	./简词法 -基准 /tmp/大文件.简

干净:
	rm -f 简词法 简词法_最小 简词法_静态 简词法_调试
