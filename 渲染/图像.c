/* ============================================================================
 *  简渲 · 图像（图像.c）
 *  ----------------------------------------------------------------------------
 *  自己写 PNG，不链接 libpng。原因和词法器不链接标准库一样：
 *  一个 40 KB 的渲染器不该拖进来一个 200 KB 的压缩库。
 *
 *  PNG 的三层：
 *      像素 → 过滤器（Paeth）→ DEFLATE（LZ77 + 固定哈夫曼）→ zlib 包装 → 分块
 *
 *  DEFLATE 只做固定哈夫曼，不做动态哈夫曼：代码少 200 行，
 *  压缩率差 5%，对渲染图（大片平滑渐变）几乎看不出来。
 * ========================================================================== */

#ifndef 简_渲_图像
#define 简_渲_图像

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- CRC / 校验 --
 */
static uint32_t crc表[256];
static int crc备好 = 0;

static void 建crc表(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc表[n] = c;
    }
    crc备好 = 1;
}

static uint32_t crc32算(const unsigned char *p, size_t n)
{
    if (!crc备好) 建crc表();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = crc表[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static uint32_t adler32算(const unsigned char *p, size_t n)
{
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = n > 5552 ? 5552 : n;
        n -= k;
        while (k--) { a += *p++; b += a; }
        a %= 65521u;
        b %= 65521u;
    }
    return (b << 16) | a;
}

/* -------------------------------------------------------------- 位写入 --
 * DEFLATE 的位是"低位先出"，而哈夫曼码本身是高位先写的，
 * 所以写码之前要把它反转一次。
 */
typedef struct {
    unsigned char *p;
    size_t 数, 容;
    uint32_t 位;
    int 位数;
} 位流;

static void 位流初始化(位流 *b, size_t 容)
{
    b->p = (unsigned char *)malloc(容);
    b->数 = 0;
    b->容 = 容;
    b->位 = 0;
    b->位数 = 0;
}

static void 位流写(位流 *b, uint32_t v, int n)
{
    b->位 |= (v & ((1u << n) - 1u)) << b->位数;
    b->位数 += n;
    while (b->位数 >= 8) {
        if (b->数 == b->容) {
            b->容 *= 2;
            b->p = (unsigned char *)realloc(b->p, b->容);
        }
        b->p[b->数++] = (unsigned char)(b->位 & 0xFF);
        b->位 >>= 8;
        b->位数 -= 8;
    }
}

static void 位流收尾(位流 *b)
{
    if (b->位数 > 0) {
        if (b->数 == b->容) {
            b->容 += 16;
            b->p = (unsigned char *)realloc(b->p, b->容);
        }
        b->p[b->数++] = (unsigned char)(b->位 & 0xFF);
        b->位 = 0;
        b->位数 = 0;
    }
}

static uint32_t 反转(uint32_t v, int n)
{
    uint32_t r = 0;
    for (int i = 0; i < n; i++) { r = (r << 1) | (v & 1); v >>= 1; }
    return r;
}

/* 固定哈夫曼表，按 RFC 1951 第 3.2.6 节 */
static void 写字面(位流 *b, int 符)
{
    if (符 <= 143)      位流写(b, 反转(0x30 + (uint32_t)符, 8), 8);
    else if (符 <= 255) 位流写(b, 反转(0x190 + (uint32_t)(符 - 144), 9), 9);
    else if (符 <= 279) 位流写(b, 反转((uint32_t)(符 - 256), 7), 7);
    else                位流写(b, 反转(0xC0 + (uint32_t)(符 - 280), 8), 8);
}

static const int 长基[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                             35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const int 长加[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                             3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const int 离基[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                             193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                             4097, 6145, 8193, 12289, 16385, 24577};
static const int 离加[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                             6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void 写匹配(位流 *b, int 长, int 距)
{
    int i = 28;
    while (i > 0 && 长基[i] > 长) i--;
    写字面(b, 257 + i);
    if (长加[i]) 位流写(b, (uint32_t)(长 - 长基[i]), 长加[i]);

    int j = 29;
    while (j > 0 && 离基[j] > 距) j--;
    位流写(b, 反转((uint32_t)j, 5), 5);
    if (离加[j]) 位流写(b, (uint32_t)(距 - 离基[j]), 离加[j]);
}

/* ------------------------------------------------------------- LZ77 --
 * 哈希链：三个字节算出槽位，同槽的按时间串成链表。
 * 只往前看 32 KB、每条链最多走 48 步——参数调大压缩率涨一点点，
 * 速度掉一大截，渲染图不缺这几个字节。
 */
#define 窗口 32768
#define 最长 258
#define 链长 48

static void 压缩(位流 *b, const unsigned char *源, size_t n)
{
    static int 头[65536];
    static int 前[窗口];
    memset(头, -1, sizeof(头));

    位流写(b, 1, 1); /* BFINAL */
    位流写(b, 1, 2); /* BTYPE = 固定哈夫曼 */

    size_t i = 0;
    while (i < n) {
        int 最佳长 = 0, 最佳距 = 0;
        if (i + 2 < n) {
            uint32_t h = (((uint32_t)源[i] << 10) ^ ((uint32_t)源[i + 1] << 5) ^ 源[i + 2]) & 0xFFFF;
            int 候选 = 头[h];
            int 步 = 0;
            int 上限 = (int)(n - i);
            if (上限 > 最长) 上限 = 最长;
            while (候选 >= 0 && 步 < 链长) {
                int 距 = (int)(i - (size_t)候选);
                if (距 <= 0 || 距 > 窗口) break;
                if (源[候选 + 最佳长] == 源[i + 最佳长]) {
                    int l = 0;
                    while (l < 上限 && 源[候选 + l] == 源[i + l]) l++;
                    if (l > 最佳长) {
                        最佳长 = l;
                        最佳距 = 距;
                        if (l >= 上限) break;
                    }
                }
                候选 = 前[候选 & (窗口 - 1)];
                步++;
            }
            /* 记进哈希链 */
            前[i & (窗口 - 1)] = 头[h];
            头[h] = (int)i;
        }

        if (最佳长 >= 3) {
            写匹配(b, 最佳长, 最佳距);
            /* 跳过的字节也要进哈希表，否则后面的匹配找不到它们 */
            for (int k = 1; k < 最佳长; k++) {
                size_t j = i + (size_t)k;
                if (j + 2 >= n) break;
                uint32_t h = (((uint32_t)源[j] << 10) ^ ((uint32_t)源[j + 1] << 5) ^ 源[j + 2]) & 0xFFFF;
                前[j & (窗口 - 1)] = 头[h];
                头[h] = (int)j;
            }
            i += (size_t)最佳长;
        } else {
            写字面(b, 源[i]);
            i++;
        }
    }
    写字面(b, 256); /* 结束块 */
    位流收尾(b);
}

/* --------------------------------------------------------------- PNG --
 */
static void 写大端(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static int 写块(FILE *f, const char *型, const unsigned char *数据, size_t 长)
{
    unsigned char 头[8];
    写大端(头, (uint32_t)长);
    memcpy(头 + 4, 型, 4);
    if (fwrite(头, 1, 8, f) != 8) return 0;
    if (长 && fwrite(数据, 1, 长, f) != 长) return 0;
    unsigned char *全 = (unsigned char *)malloc(长 + 4);
    if (!全) return 0;
    memcpy(全, 型, 4);
    if (长) memcpy(全 + 4, 数据, 长);
    uint32_t c = crc32算(全, 长 + 4);
    free(全);
    unsigned char 尾[4];
    写大端(尾, c);
    return fwrite(尾, 1, 4, f) == 4;
}

static inline int 像素预测(int a, int b, int c)
{
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static int 写PNG(const char *路径, const unsigned char *rgb, int 宽, int 高)
{
    size_t 行字节 = (size_t)宽 * 3;
    size_t 原长 = (行字节 + 1) * (size_t)高;
    unsigned char *原 = (unsigned char *)malloc(原长);
    if (!原) return 0;

    unsigned char *暂 = (unsigned char *)malloc(行字节);
    unsigned char *最优 = (unsigned char *)malloc(行字节);
    if (!暂 || !最优) { free(原); free(暂); free(最优); return 0; }

    for (int y = 0; y < 高; y++) {
        const unsigned char *行 = rgb + (size_t)y * 行字节;
        const unsigned char *上行 = y > 0 ? rgb + (size_t)(y - 1) * 行字节 : NULL;

        int 最优型 = 0;
        long 最优和 = -1;

        /* 试五种过滤器，选绝对值和最小的那个（PNG 官方推荐的启发式） */
        for (int 型 = 0; 型 < 5; 型++) {
            long 和 = 0;
            for (size_t i = 0; i < 行字节; i++) {
                int a = i >= 3 ? 行[i - 3] : 0;
                int b = 上行 ? 上行[i] : 0;
                int c = (上行 && i >= 3) ? 上行[i - 3] : 0;
                int 值;
                switch (型) {
                case 0: 值 = 行[i]; break;
                case 1: 值 = 行[i] - a; break;
                case 2: 值 = 行[i] - b; break;
                case 3: 值 = 行[i] - ((a + b) >> 1); break;
                default: 值 = 行[i] - 像素预测(a, b, c); break;
                }
                暂[i] = (unsigned char)(值 & 0xFF);
                和 += 值 < 0 ? -值 : 值;
            }
            if (最优和 < 0 || 和 < 最优和) {
                最优和 = 和;
                最优型 = 型;
                memcpy(最优, 暂, 行字节);
            }
        }
        原[(行字节 + 1) * (size_t)y] = (unsigned char)最优型;
        memcpy(原 + (行字节 + 1) * (size_t)y + 1, 最优, 行字节);
    }
    free(暂);
    free(最优);

    位流 b;
    位流初始化(&b, 原长 / 2 + 1024);
    压缩(&b, 原, 原长);

    uint32_t ad = adler32算(原, 原长);
    free(原);

    /* zlib 包装：0x78 0x01（最快压缩）+ deflate + adler32 */
    size_t z长 = b.数 + 6;
    unsigned char *z = (unsigned char *)malloc(z长);
    if (!z) { free(b.p); return 0; }
    z[0] = 0x78;
    z[1] = 0x01;
    memcpy(z + 2, b.p, b.数);
    写大端(z + 2 + b.数, ad);
    free(b.p);

    FILE *f = fopen(路径, "wb");
    if (!f) { free(z); return 0; }

    static const unsigned char 签[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    int 好 = fwrite(签, 1, 8, f) == 8;

    unsigned char ihdr[13];
    写大端(ihdr, (uint32_t)宽);
    写大端(ihdr + 4, (uint32_t)高);
    ihdr[8] = 8;   /* 位深 */
    ihdr[9] = 2;   /* 颜色类型：真彩色 */
    ihdr[10] = 0;  /* 压缩 */
    ihdr[11] = 0;  /* 过滤 */
    ihdr[12] = 0;  /* 隔行 */
    好 = 好 && 写块(f, "IHDR", ihdr, 13);
    好 = 好 && 写块(f, "IDAT", z, z长);
    好 = 好 && 写块(f, "IEND", NULL, 0);
    fclose(f);
    free(z);
    return 好;
}

#endif
