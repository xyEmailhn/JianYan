#ifndef LIBMPDEC_PYCONFIG_H_
#define LIBMPDEC_PYCONFIG_H_
/*
 * cpython 的 libmpdec 副本要求外部提供这个头（上游 mpdecimal 由 configure
 * 探测，不依赖它）。这里把它需要的四个编译期事实一次说清，省得每个引用
 * mpdecimal.h 的文件都得在命令行上重复一遍。
 *
 * CONFIG_64 / ANSI  —— 64 位字长、纯 C（不走汇编）
 * HAVE_UINT128_T    —— x86-64 有 __uint128_t，双字乘除可走内联快路径
 * HAVE_UINT64_T     —— 有 uint64_t，模运算可走 std_mulmod 而非逐位循环
 */
#define CONFIG_64 1
#define ANSI 1
#define HAVE_UINT128_T 1
#define HAVE_UINT64_T 1
#endif
