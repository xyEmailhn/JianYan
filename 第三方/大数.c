/*
 * 大数.c —— libtommath 与 mpdecimal 的胶水层。
 *
 * 边界跟 运行库.c 一样：这一层不认识简言的任何内部结构，只收裸指针和
 * 裸字符串；反过来，生成的 C 也看不见 tommath.h / mpdecimal.h，两边
 * 都用 void * 打招呼。
 *
 * 语义约定（语言层依赖这三条，改之前先看规范第 13 节）：
 *
 *   1. 不可变。所有运算都返回新对象，绝不改动入参。因此一个指针可以被
 *      多个变量持有而不出事——赋值只是复制 8 字节。
 *   2. 不回收。没有引用计数，对象活到进程结束（跟 图 一样）。
 *   3. 出错不崩。整数除零得 0，解析失败由调用方看 错 缓冲区。
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "tommath/tommath.h"
#include "mpdec/mpdecimal.h"

/* ------------------------------------------------------------------ */
/*  小工具                                                             */
/* ------------------------------------------------------------------ */

static char *补零拷贝(const char *s, long long 长)
{
    char *t;
    if (长 < 0) return NULL;
    t = (char *)malloc((size_t)长 + 1);
    if (!t) return NULL;
    if (长) memcpy(t, s, (size_t)长);
    t[长] = '\0';
    return t;
}

/* 前导空白跳过，返回新的起点；顺带把长度改短 */
static const char *去空白(const char *s, long long *长)
{
    while (*长 > 0 && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) {
        s++; (*长)--;
    }
    while (*长 > 0 && (s[*长 - 1] == ' ' || s[*长 - 1] == '\t' ||
                       s[*长 - 1] == '\n' || s[*长 - 1] == '\r')) {
        (*长)--;
    }
    return s;
}

/* 看前缀定进制：0x/0X 十六，0b/0B 二进制，0o/0O 八进制，其余十 */
static int 判进制(const char *s, long long *长, long long *偏移)
{
    *偏移 = 0;
    if (*长 >= 2 && s[0] == '0') {
        switch (s[1]) {
        case 'x': case 'X': *偏移 = 2; return 16;
        case 'b': case 'B': *偏移 = 2; return 2;
        case 'o': case 'O': *偏移 = 2; return 8;
        default: break;
        }
    }
    return 10;
}

/* ------------------------------------------------------------------ */
/*  大整数：void * 指向一块 malloc 出来的 mp_int                        */
/* ------------------------------------------------------------------ */

static mp_int *新大整(void)
{
    mp_int *p = (mp_int *)malloc(sizeof(mp_int));
    if (!p) return NULL;
    if (mp_init(p) != MP_OKAY) { free(p); return NULL; }
    return p;
}

static void 放大整(void *p)
{
    if (p) { mp_clear((mp_int *)p); free(p); }
}

void *简_大_从整(long long v)
{
    mp_int *r = 新大整();
    if (!r) return NULL;
    mp_set_i64(r, (int64_t)v);        /* 1.3 起返回 void，不再有失败可能 */
    return r;
}

void *简_大_读(const char *s, long long 长, char *错)
{
    const char *起;
    char *tmp;
    mp_int *r;
    long long 偏;
    int 进制;
    mp_err e;

    if (错) 错[0] = '\0';
    起 = 去空白(s, &长);
    if (长 == 0) {
        if (错) snprintf(错, 256, "空字符串不是大整数");
        return NULL;
    }
    进制 = 判进制(起, &长, &偏);

    tmp = 补零拷贝(起 + 偏, 长 - 偏);
    if (!tmp) { if (错) snprintf(错, 256, "内存不够"); return NULL; }

    r = 新大整();
    if (!r) { free(tmp); if (错) snprintf(错, 256, "内存不够"); return NULL; }

    e = mp_read_radix(r, tmp, 进制);
    if (e != MP_OKAY) {
        放大整(r);
        if (错) snprintf(错, 256, "\"%s\" 不是合法的 %d 进制整数", tmp, 进制);
        free(tmp);
        return NULL;
    }
    free(tmp);
    return r;
}

#define 二元大(名, 调用)                                                  \
    void *名(const void *a, const void *b) {                               \
        mp_int *r = 新大整();                                              \
        if (!r) return NULL;                                               \
        if (调用 != MP_OKAY) { 放大整(r); return NULL; }                    \
        return r;                                                          \
    }

二元大(简_大_加, mp_add((const mp_int *)a, (const mp_int *)b, r))
二元大(简_大_减, mp_sub((const mp_int *)a, (const mp_int *)b, r))
二元大(简_大_乘, mp_mul((const mp_int *)a, (const mp_int *)b, r))

void *简_大_除(const void *a, const void *b)
{
    const mp_int *x = (const mp_int *)a, *y = (const mp_int *)b;
    mp_int *r = 新大整();
    if (!r) return NULL;
    if (mp_iszero(y)) { mp_zero(r); return r; }   /* 整数除零：定义为 0 */
    if (mp_div(x, y, r, NULL) != MP_OKAY) { 放大整(r); return NULL; }
    return r;
}

void *简_大_余(const void *a, const void *b)
{
    const mp_int *x = (const mp_int *)a, *y = (const mp_int *)b;
    mp_int *r = 新大整();
    if (!r) return NULL;
    if (mp_iszero(y)) { mp_zero(r); return r; }
    if (mp_div(x, y, NULL, r) != MP_OKAY) { 放大整(r); return NULL; }
    return r;
}

/* 指数上限：2^10000000 约 3.7 MB，再大就是拿内存换死机 */
#define 幂上限 10000000LL

/* 出错时给 0 而不是 NULL：语言层没有错误通道，一个 NULL 会在下一步被解引用。
 * 宁可给一个能继续算的值，把语义写在文档里（负数开方得 0、指数越界得 0）。 */
static mp_int *零大整(void)
{
    mp_int *r = 新大整();
    if (r) mp_zero(r);
    return r;
}

void *简_大_幂(const void *a, long long e)
{
    mp_int *r;
    if (e < 0 || e > 幂上限) return 零大整();
    r = 新大整();
    if (!r) return NULL;
    if (mp_expt_n((const mp_int *)a, (int)e, r) != MP_OKAY) { 放大整(r); return 零大整(); }
    return r;
}

void *简_大_根(const void *a)
{
    const mp_int *x = (const mp_int *)a;
    mp_int *r;
    if (mp_isneg(x)) return 零大整();
    r = 新大整();
    if (!r) return NULL;
    if (mp_sqrt(x, r) != MP_OKAY) { 放大整(r); return 零大整(); }
    return r;
}

long long 简_大_比(const void *a, const void *b)
{
    mp_ord c = mp_cmp((const mp_int *)a, (const mp_int *)b);
    if (c == MP_LT) return -1;
    if (c == MP_GT) return 1;
    return 0;
}

/* 十进制有多少位（不含负号） */
long long 简_大_位(const void *a)
{
    const mp_int *x = (const mp_int *)a;
    int sz = 0;
    if (mp_radix_size(x, 10, &sz) != MP_OKAY) return 0;
    return (long long)sz - 1 - (mp_isneg(x) ? 1 : 0);
}

char *简_大_文(const void *a)
{
    const mp_int *x = (const mp_int *)a;
    int sz = 0;
    size_t n = 0;
    char *s;

    if (mp_radix_size(x, 10, &sz) != MP_OKAY || sz <= 0) return NULL;
    s = (char *)malloc((size_t)sz + 1);
    if (!s) return NULL;
    if (mp_to_radix(x, s, (size_t)sz + 1, &n, 10) != MP_OKAY) { free(s); return NULL; }
    s[n] = '\0';
    return s;
}

void 简_大_放(void *p) { 放大整(p); }

/* ------------------------------------------------------------------ */
/*  小数：void * 指向 mpd_t（十进制、任意精度、正确舍入）              */
/* ------------------------------------------------------------------ */

static mpd_context_t 上下文;
static int 上下文好 = 0;

static void 备上下文(void)
{
    if (上下文好) return;
    mpd_init(&上下文, 34);        /* decimal128 的精度 */
    上下文.traps = 0;             /* 只记 status，不发信号 */
    上下文好 = 1;
}

void 简_小_定精度(long long 位)
{
    备上下文();
    if (位 < 1) 位 = 1;
    if (位 > 1000000) 位 = 1000000;
    上下文.prec = (mpd_ssize_t)位;
}

long long 简_小_取精度(void)
{
    备上下文();
    return (long long)上下文.prec;
}

void *简_小_从整(long long v)
{
    mpd_t *r;
    备上下文();
    r = mpd_new(&上下文);
    if (!r) return NULL;
    mpd_qset_ssize(r, (mpd_ssize_t)v, &上下文, NULL);
    return r;
}

void *简_小_从浮(double v)
{
    mpd_t *r;
    char buf[64];
    备上下文();
    /* 用 %.17g 只是为了让 double 的十进制展开稳定；真要精确请从字符串来 */
    snprintf(buf, sizeof buf, "%.17g", v);
    r = mpd_new(&上下文);
    if (!r) return NULL;
    mpd_qset_string(r, buf, &上下文, NULL);
    return r;
}

void *简_小_读(const char *s, long long 长, char *错)
{
    const char *起;
    char *tmp;
    mpd_t *r;
    uint32_t 状态 = 0;

    if (错) 错[0] = '\0';
    备上下文();
    起 = 去空白(s, &长);
    if (长 == 0) { if (错) snprintf(错, 256, "空字符串不是小数"); return NULL; }

    tmp = 补零拷贝(起, 长);
    if (!tmp) { if (错) snprintf(错, 256, "内存不够"); return NULL; }

    r = mpd_new(&上下文);
    if (!r) { free(tmp); if (错) snprintf(错, 256, "内存不够"); return NULL; }

    mpd_qset_string(r, tmp, &上下文, &状态);
    if ((状态 & MPD_Conversion_syntax) || (状态 & MPD_Invalid_operation)) {
        mpd_del(r);
        if (错) snprintf(错, 256, "\"%s\" 不是合法的小数", tmp);
        free(tmp);
        return NULL;
    }
    free(tmp);
    return r;
}

#define 二元小(名, 函数)                                                  \
    void *名(const void *a, const void *b) {                               \
        mpd_t *r;                                                          \
        uint32_t 状态 = 0;                                                 \
        备上下文();                                                        \
        r = mpd_new(&上下文);                                              \
        if (!r) return NULL;                                               \
        函数(r, (const mpd_t *)a, (const mpd_t *)b, &上下文, &状态);        \
        return r;                                                          \
    }

二元小(简_小_加, mpd_qadd)
二元小(简_小_减, mpd_qsub)
二元小(简_小_乘, mpd_qmul)

/* 除零：mpdecimal 给 Infinity，不崩，由语言层原样输出 */
二元小(简_小_除, mpd_qdiv)

long long 简_小_比(const void *a, const void *b)
{
    uint32_t 状态 = 0;
    int 序;
    备上下文();
    序 = mpd_qcmp((const mpd_t *)a, (const mpd_t *)b, &状态);
    if (状态 & MPD_Invalid_operation) return 0;   /* 含 NaN 的情况，定为相等 */
    if (序 < 0) return -1;
    if (序 > 0) return 1;
    return 0;
}

char *简_小_文(const void *a)
{
    const mpd_t *x = (const mpd_t *)a;
    char *s;

    if (mpd_isnan(x))  { s = (char *)malloc(7);  if (s) memcpy(s, "非数", 7);  return s; }
    if (mpd_isinfinite(x)) {
        const char *t = mpd_issigned(x) ? "-无穷" : "无穷";
        size_t n = strlen(t);
        s = (char *)malloc(n + 1);
        if (s) memcpy(s, t, n + 1);
        return s;
    }

    /* 默认输出定点写法：1e10 要写 10000000000，不是 1e+10。
     * 但 1e-100000 这种若也展开就是十万个零，所以超长时退回科学计数法——
     * 宁可难看，不要卡死。阈值取 1000 是因为 1000 位的高精度算例确实存在。 */
    s = mpd_qformat(x, "f", &上下文, NULL);
    if (!s) return mpd_to_sci(x, 0);
    if (strlen(s) > 1000) { mpd_free(s); return mpd_to_sci(x, 0); }
    return s;
}

void 简_小_放(void *p) { if (p) mpd_del((mpd_t *)p); }
