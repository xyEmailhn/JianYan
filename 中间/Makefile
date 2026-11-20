# 简言 · 中间表示与单态化 构建脚本
#
#   make          速度最快（动态链接）
#   make 最小      体积最小且完全自包含（musl 静态链接）
#   make 静态      静态链接的速度版
#   make 调试      带符号、不优化
#   make 跑        跑一遍示例
#   make 测        造一份大源码，测吞吐
#   make 比折叠    同一个函数，折叠与不折叠并排看

CC = gcc

ZIG = $(shell command -v zig 2>/dev/null || ls /usr/local/lib/python3*/dist-packages/ziglang/zig 2>/dev/null | head -1)

快 = -O3 -flto -fno-plt -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident -ffunction-sections -Wl,--gc-sections

.PHONY: all 最小 静态 调试 跑 测 比折叠 可视化 干净

all: 简中间

简中间: ir.c ../语义/sema.c ../语法/parser.c ../词法/lexer.c
	$(CC) $(快) -s -o $@ ir.c -lm

最小: ir.c
	@test -n "$(ZIG)" || (echo "需要 zig：pip3 install --break-system-packages ziglang"; false)
	$(ZIG) cc -target x86_64-linux-musl -Oz -s -o 简中间_最小 ir.c -lm

静态: ir.c
	@test -n "$(ZIG)" || (echo "需要 zig：pip3 install --break-system-packages ziglang"; false)
	$(ZIG) cc -target x86_64-linux-musl -O3 -s -o 简中间_静态 ir.c -lm

调试: ir.c
	$(CC) -O0 -g -o 简中间_调试 ir.c -lm

跑: 简中间
	@./简中间 示例/泛型.jy
	@echo "--- 语义有错的文件会被拦下 ---"
	@./简中间 ../语义/示例/错误.jy 2>&1 | tail -2 || true

测: 简中间
	@python3 -c "L=['模块 基准','结构 点 {','    x: 整数','    y: 整数','}','函数 最大[T: 可比较](甲: T, 乙: T) -> T {','    如果 甲 >> 乙 就 { 返回 甲 }','    返回 乙','}','函数 大函数() -> 整数 {','    私有 整数 累加 = 0'];L+=['    累加 = 累加 + %d %% 7'%(i%97) for i in range(12000)];L+=['    返回 累加','}'];L+=['函数 演示() {'];L+=['    私有 整数 变量%d = 最大(%d, %d)'%(i,i,i+1) for i in range(200)];L+=['}'];open('/tmp/中间大.jy','w',encoding='utf-8').write('\n'.join(L))"
	@./简中间 -基准 /tmp/中间大.jy

比折叠: 简中间
	@echo "=== 折叠后 ==="
	@./简中间 示例/泛型.jy 2>/dev/null | sed -n '/函数 折叠/,/^$$/p'
	@echo "=== 不折叠 ==="
	@./简中间 -不折叠 示例/泛型.jy 2>/dev/null | sed -n '/函数 折叠/,/^$$/p'

可视化: 简中间
	@./简中间 示例/覆盖.jy 2>/dev/null | python3 生成可视化.py -o 示例/覆盖_中间表示.html
	@./简中间 示例/泛型.jy 2>/dev/null | python3 生成可视化.py -o 示例/泛型_中间表示.html

干净:
	rm -f 简中间 简中间_最小 简中间_静态 简中间_调试
