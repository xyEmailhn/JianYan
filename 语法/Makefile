# 简言 · 语法分析器 构建脚本
#
# 四个目标，按需要选：
#   make          速度最快（动态链接）
#   make 最小      体积最小且完全自包含（musl 静态链接，拷到任何 Linux 都能跑）
#   make 静态      静态链接的速度版
#   make 调试      带符号、不优化，出问题用它
#
# 说明：最小/静态两个目标需要 zig（它自带 musl 运行库）。
#       没装 zig 也不影响 `make`，只是静态版打不出来。
#
# 解析器把词法器整个 #include 进来，所以这里只编译 parser.c 一个文件。

CC = gcc

ZIG = $(shell command -v zig 2>/dev/null || ls /usr/local/lib/python3*/dist-packages/ziglang/zig 2>/dev/null | head -1)

快 = -O3 -flto -fno-plt -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident

.PHONY: all 最小 静态 调试 跑 看树 对答案 测 干净

all: 简语法

简语法: parser.c ../词法/lexer.c
	$(CC) $(快) -s -o $@ parser.c

最小: parser.c ../词法/lexer.c
	@test -n "$(ZIG)" || (echo "需要 zig：pip3 install --break-system-packages ziglang"; false)
	$(ZIG) cc -target x86_64-linux-musl -Oz -s -o 简语法_最小 parser.c

静态: parser.c ../词法/lexer.c
	@test -n "$(ZIG)" || (echo "需要 zig：pip3 install --break-system-packages ziglang"; false)
	$(ZIG) cc -target x86_64-linux-musl -O3 -s -o 简语法_静态 parser.c

调试: parser.c
	$(CC) -O0 -g -o 简语法_调试 parser.c

跑: 简语法
	./简语法 示例/覆盖.简

# 打印语法树
看树: 简语法
	./简语法 ../词法/示例/词频.简

# C 版和 Python 版各出一份树，逐字节比对
对答案: 简语法
	./简语法 ../词法/示例/词频.简 > /tmp/c树.txt 2>/dev/null
	python3 parser_参考实现.py ../词法/示例/词频.简 > /tmp/py树.txt 2>/dev/null
	@diff /tmp/c树.txt /tmp/py树.txt && echo "两份实现输出完全一致"

测: 简语法
	@python3 -c "s=open('示例/覆盖.简',encoding='utf-8').read();open('/tmp/大源码.简','w',encoding='utf-8').write(s*2500)"
	./简语法 -基准 /tmp/大源码.简

干净:
	rm -f 简语法 简语法_最小 简语法_静态 简语法_调试
