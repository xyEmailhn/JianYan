# 简言 · 第三方库构建
#
#   make          生成 lib简言图.a
#   make 干净      清掉产物
#
# 这里的产物不提交进仓库。简编译 找不到它时，纯计算程序照样能编，
# 只有用到 `图` / `大整数` / `小数` 的程序会在链接阶段报 undefined reference，
# 那时提示里会告诉你回这个目录跑 make。
#
# 注意 ar 只打包不链接：171 个目标文件进一个 .a，链接时按需取成员，
# 只用 `图` 的程序不会被拖进 tommath 的符号。

CC = gcc
CFLAGS = -O2 -w -fno-plt

# libtommath：154 个 bn_*.c，每个函数一个文件，上游就这么发的
TOM_C := $(sort $(wildcard tommath/bn_*.c))
TOM_O := $(TOM_C:.c=.o)

# mpdecimal：15 个 .c，头文件里的 CONFIG_64/ANSI 由 mpdec/pyconfig.h 提供
MPD_C := $(sort $(wildcard mpdec/*.c))
MPD_O := $(MPD_C:.c=.o)

.PHONY: all 干净

all: lib简言图.a

lib简言图.a: 运行库.o 大数.o 声音.o $(TOM_O) $(MPD_O)
	@rm -f $@
	ar rcs $@ $^

运行库.o: 运行库.c stb/stb_image.h stb/stb_image_write.h stb/stb_truetype.h \
          stb/stb_rect_pack.h nanosvg/nanosvg.h nanosvg/nanosvgrast.h
	$(CC) $(CFLAGS) -c 运行库.c -o $@

大数.o: 大数.c tommath/tommath.h tommath/tommath_private.h mpdec/mpdecimal.h mpdec/pyconfig.h
	$(CC) $(CFLAGS) -c 大数.c -o $@

声音.o: 声音.c dr/dr_wav.h dr/dr_mp3.h
	$(CC) $(CFLAGS) -c 声音.c -o $@

tommath/%.o: tommath/%.c
	$(CC) $(CFLAGS) -c $< -o $@

mpdec/%.o: mpdec/%.c
	$(CC) $(CFLAGS) -c $< -o $@

干净:
	rm -f 运行库.o 大数.o 声音.o lib简言图.a $(TOM_O) $(MPD_O)
