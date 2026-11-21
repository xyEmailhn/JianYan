# 简言 · 第三方库构建
#
#   make          生成 lib简言图.a
#   make 干净      清掉产物
#
# 这里的产物不提交进仓库。简编译 找不到它时，纯计算程序照样能编，
# 只有用到 `图` 的程序会在链接阶段报 undefined reference，
# 那时提示里会告诉你回这个目录跑 make。

CC = gcc
CFLAGS = -O2 -w -fno-plt

.PHONY: all 干净

all: lib简言图.a

lib简言图.a: 运行库.o
	ar rcs $@ $^

运行库.o: 运行库.c stb/stb_image.h stb/stb_image_write.h stb/stb_truetype.h \
          stb/stb_rect_pack.h nanosvg/nanosvg.h nanosvg/nanosvgrast.h
	$(CC) $(CFLAGS) -c 运行库.c -o $@

干净:
	rm -f 运行库.o lib简言图.a
