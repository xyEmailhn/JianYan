/* ============================================================================
 *  简言 · 图形运行时（第三方库的胶水层）
 *  ----------------------------------------------------------------------------
 *  这里的代码**不随生成的 .c 一起走**，而是预先编成 lib简言图.a。
 *
 *  为什么分开：stb_image + stb_image_write + stb_truetype 加起来四十多万行，
 *  每个 .jy 程序都编一遍要一秒多。编成静态库之后，链接只取用到的目标文件，
 *  生成的 C 里只剩几行 extern 声明。
 *
 *  ---- 接口约定 ----
 *
 *  像素一律 RGBA8、行紧凑（每行正好 宽*4 字节）、左上角原点。
 *  所有函数都不认识「简图」这个结构体，只收 指针 + 宽 + 高。
 *  这样运行时库和生成的 C 之间没有共享头文件，谁先编都行。
 * ========================================================================== */

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_RECT_PACK_IMPLEMENTATION
#define STB_TRUETYPE_IMPLEMENTATION

#include "stb/stb_image.h"
#include "stb/stb_image_write.h"
#include "stb/stb_rect_pack.h"
#include "stb/stb_truetype.h"

#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------------ 工具 -- */

static const char *取扩展名(const char *路径)
{
    const char *点 = NULL;
    for (const char *p = 路径; *p; p++) {
        if (*p == '.') 点 = p;
        else if (*p == '/' || *p == '\\') 点 = NULL;   /* 目录里的点不算 */
    }
    return 点;
}

/* ------------------------------------------------------------------ 读图 -- */

/* 成功返回 malloc 出来的像素缓冲，失败返回 NULL 并把原因写进 错。
 * 错 是调用方给的缓冲，至少 256 字节。 */
unsigned char *简_图_载(const char *路径, long long *宽, long long *高, char *错)
{
    if (!路径 || !*路径) {
        if (错) snprintf(错, 256, "路径是空的");
        return NULL;
    }

    const char *ext = 取扩展名(路径);

    /* SVG 走 nanosvg：先解析成路径，再自己栅格化。
     * 这让 `读图("x.svg")` 和 `读图("x.png")` 用起来一模一样。 */
    if (ext && strcasecmp(ext, ".svg") == 0) {
        NSVGimage *图 = nsvgParseFromFile(路径, "px", 96.0f);
        if (!图) {
            if (错) snprintf(错, 256, "SVG 解析失败：%s", 路径);
            return NULL;
        }
        int w = (int)(图->width  + 0.5f);
        int h = (int)(图->height + 0.5f);
        if (w <= 0 || h <= 0) { nsvgDelete(图); if (错) snprintf(错, 256, "SVG 没有尺寸"); return NULL; }
        /* +4 行余量：路径描边常常溢出画布，多给一点免得被裁 */
        unsigned char *像素 = (unsigned char *)calloc((size_t)w * (size_t)h * 4u, 1);
        if (!像素) { nsvgDelete(图); if (错) snprintf(错, 256, "内存不够"); return NULL; }
        NSVGrasterizer *光栅 = nsvgCreateRasterizer();
        if (!光栅) { free(像素); nsvgDelete(图); if (错) snprintf(错, 256, "光栅化器创建失败"); return NULL; }
        nsvgRasterize(光栅, 图, 0, 0, 1.0f, 像素, w, h, w * 4);
        nsvgDeleteRasterizer(光栅);
        nsvgDelete(图);
        *宽 = w; *高 = h;
        return 像素;
    }

    int w = 0, h = 0, 通道 = 0;
    /* 第 4 个参数写 4 = 强制要 RGBA。上层所有代码都按 4 字节一个像素算，
     * 让 stb 在这里做一次转换，比每个调用点都判断通道数省事得多。 */
    unsigned char *像素 = stbi_load(路径, &w, &h, &通道, 4);
    if (!像素) {
        if (错) {
            const char *因 = stbi_failure_reason();
            snprintf(错, 256, "读不了这个文件：%s", 因 ? 因 : "格式不认识");
        }
        return NULL;
    }
    *宽 = w; *高 = h;
    return 像素;
}

/* ------------------------------------------------------------------ 存图 -- */

/* 按扩展名挑编码器。返回 1 成功。 */
int 简_图_存(const char *路径, const unsigned char *像素, long long 宽, long long 高)
{
    if (!路径 || !像素 || 宽 <= 0 || 高 <= 0) return 0;
    int w = (int)宽, h = (int)高;
    const char *ext = 取扩展名(路径);
    if (!ext) return 0;

    if (strcasecmp(ext, ".png")  == 0) return stbi_write_png(路径, w, h, 4, 像素, w * 4);
    if (strcasecmp(ext, ".jpg")  == 0 ||
        strcasecmp(ext, ".jpeg") == 0) return stbi_write_jpg(路径, w, h, 4, 像素, 92);
    if (strcasecmp(ext, ".bmp")  == 0) return stbi_write_bmp(路径, w, h, 4, 像素);
    if (strcasecmp(ext, ".tga")  == 0) return stbi_write_tga(路径, w, h, 4, 像素);
    return 0;   /* 不认识的扩展名就明说失败，别悄悄存成别的东西 */
}

/* ---------------------------------------------------------------- 新建 -- */

unsigned char *简_图_新(long long 宽, long long 高)
{
    if (宽 <= 0 || 高 <= 0) return NULL;
    size_t n = (size_t)宽 * (size_t)高 * 4u;
    unsigned char *像素 = (unsigned char *)malloc(n);
    if (!像素) return NULL;
    /* 不透明黑。透明画布看着"干净"，但画东西时忘了写 alpha 就会得到一张
     * 全透明的图，打开一看一片空白——那个坑不值当。 */
    memset(像素, 0, n);
    for (size_t i = 3; i < n; i += 4) 像素[i] = 255;
    return 像素;
}

void 简_图_放(unsigned char *像素)
{
    free(像素);
}

/* ---------------------------------------------------------------- 缩放 -- */

/* 双线性。放大缩小时都还过得去，写起来只有二十行。
 * 用定点坐标算权重：整数运算比浮点快，也不会有累积误差。 */
unsigned char *简_图_缩(const unsigned char *源, long long 宽, long long 高,
                        long long 新宽, long long 新高)
{
    if (!源 || 宽 <= 0 || 高 <= 0 || 新宽 <= 0 || 新高 <= 0) return NULL;
    unsigned char *出 = (unsigned char *)malloc((size_t)新宽 * (size_t)新高 * 4u);
    if (!出) return NULL;

    for (long long y = 0; y < 新高; y++) {
        /* 采样点落在源图的哪儿。减 0.5 是"像素中心对齐"：
         * 不做这一步，放大出来会整体偏左上半个像素。 */
        double sy = ((double)y + 0.5) * (double)高 / (double)新高 - 0.5;
        long long y0 = (long long)sy;
        if (sy < 0) y0 = 0;
        long long y1 = y0 + 1 < 高 ? y0 + 1 : 高 - 1;
        if (y0 >= 高) y0 = 高 - 1;
        double fy = sy - (double)y0; if (fy < 0) fy = 0; if (fy > 1) fy = 1;

        for (long long x = 0; x < 新宽; x++) {
            double sx = ((double)x + 0.5) * (double)宽 / (double)新宽 - 0.5;
            long long x0 = (long long)sx;
            if (sx < 0) x0 = 0;
            long long x1 = x0 + 1 < 宽 ? x0 + 1 : 宽 - 1;
            if (x0 >= 宽) x0 = 宽 - 1;
            double fx = sx - (double)x0; if (fx < 0) fx = 0; if (fx > 1) fx = 1;

            const unsigned char *p00 = 源 + ((size_t)y0 * 宽 + x0) * 4;
            const unsigned char *p10 = 源 + ((size_t)y0 * 宽 + x1) * 4;
            const unsigned char *p01 = 源 + ((size_t)y1 * 宽 + x0) * 4;
            const unsigned char *p11 = 源 + ((size_t)y1 * 宽 + x1) * 4;
            unsigned char *o = 出 + ((size_t)y * 新宽 + x) * 4;

            for (int c = 0; c < 4; c++) {
                double 上 = p00[c] + (p10[c] - p00[c]) * fx;
                double 下 = p01[c] + (p11[c] - p01[c]) * fx;
                double v = 上 + (下 - 上) * fy;
                o[c] = (unsigned char)(v + 0.5);
            }
        }
    }
    return 出;
}

/* ---------------------------------------------------------------- 写字 -- */

/* 字体是懒加载的：只有真的调用了 图.写字 才去读文件。
 * 不写字画出的图，不该因为机器上没装字体就编不过或跑不起来。 */
static unsigned char *字体数据 = NULL;
static stbtt_fontinfo 字体;
static int 字体就绪 = 0, 字体试过 = 0;

static unsigned char *读整个文件(const char *路径, long *长度)
{
    FILE *f = fopen(路径, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    unsigned char *p = (unsigned char *)malloc((size_t)n);
    if (!p) { fclose(f); return NULL; }
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f);
    *长度 = n;
    return p;
}

static int 备好字体(void)
{
    if (字体试过) return 字体就绪;
    字体试过 = 1;

    /* 顺序有讲究：环境变量优先（用户可以指定自己的字体），
     * 然后是常见的 CJK 字体（中文必须能画），最后兜底 DejaVu（只有拉丁）。 */
    const char *候选[] = {
        getenv("简言字体"),
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    for (int i = 0; i < (int)(sizeof(候选) / sizeof(候选[0])); i++) {
        if (!候选[i]) continue;
        long n = 0;
        unsigned char *data = 读整个文件(候选[i], &n);
        if (!data) continue;
        /* ttc 是字体集合，取第 0 个。stb 对 ttf 返回 0，对 ttc 返回偏移。 */
        int 偏移 = stbtt_GetFontOffsetForIndex(data, 0);
        if (偏移 < 0 || !stbtt_InitFont(&字体, data, 偏移)) { free(data); continue; }
        字体数据 = data;
        字体就绪 = 1;
        return 1;
    }
    return 0;
}

/* UTF-8 解一个码点。返回消耗的字节数，*码点 是结果。坏字节当 U+FFFD 跳过 1 字节。 */
static int 解一字(const unsigned char *p, long 剩, int *码点)
{
    if (剩 <= 0) { *码点 = 0; return 0; }
    unsigned char c = p[0];
    if (c < 0x80) { *码点 = c; return 1; }
    int n, v;
    if ((c & 0xE0) == 0xC0)      { n = 2; v = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 3; v = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 4; v = c & 0x07; }
    else { *码点 = 0xFFFD; return 1; }
    if (剩 < n) { *码点 = 0xFFFD; return 1; }
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *码点 = 0xFFFD; return 1; }
        v = (v << 6) | (p[i] & 0x3F);
    }
    *码点 = v;
    return n;
}

/* 把一个字合成到画布上。alpha 来自 stb 的覆盖率位图，做一次正经的 over 合成。 */
static void 画一字(unsigned char *画布, int 宽, int 高, int 处x, int 处y,
                   int w, int h, const unsigned char *罩, unsigned char r,
                   unsigned char g, unsigned char b, unsigned char a)
{
    for (int y = 0; y < h; y++) {
        int 画y = 处y + y;
        if (画y < 0 || 画y >= 高) continue;
        for (int x = 0; x < w; x++) {
            int 画x = 处x + x;
            if (画x < 0 || 画x >= 宽) continue;
            unsigned char 覆 = 罩[y * w + x];
            if (!覆) continue;
            /* 覆盖率 × 字色 alpha，再对底色做 over */
            unsigned 源a = (unsigned)覆 * (unsigned)a / 255u;
            unsigned char *d = 画布 + ((size_t)画y * (size_t)宽 + (size_t)画x) * 4;
            unsigned 反 = 255u - 源a;
            d[0] = (unsigned char)((r * 源a + d[0] * 反) / 255u);
            d[1] = (unsigned char)((g * 源a + d[1] * 反) / 255u);
            d[2] = (unsigned char)((b * 源a + d[2] * 反) / 255u);
            d[3] = (unsigned char)((255u * 源a + d[3] * 反) / 255u);
        }
    }
}

/* 返回画了几个字；-1 表示没有可用字体。 */
long long 简_图_写(unsigned char *画布, long long 宽, long long 高,
                   long long x, long long y, double 字号,
                   const char *文字, long long 长, long long 颜色)
{
    if (!画布 || 宽 <= 0 || 高 <= 0 || 字号 <= 0) return 0;
    if (!备好字体()) return -1;

    float 比 = stbtt_ScaleForPixelHeight(&字体, (float)字号);
    int 升, 降, 行距;
    stbtt_GetFontVMetrics(&字体, &升, &降, &行距);
    double 行高 = (double)(升 - 降 + 行距) * 比;

    unsigned char r = (unsigned char)((颜色 >> 24) & 255);
    unsigned char g = (unsigned char)((颜色 >> 16) & 255);
    unsigned char b = (unsigned char)((颜色 >>  8) & 255);
    unsigned char a = (unsigned char)( 颜色        & 255);

    double 笔x = (double)x;
    double 基线 = (double)y + (double)升 * 比;   /* y 是这一行的顶边，跟 CSS 一致 */
    long long 字数 = 0;

    const unsigned char *p = (const unsigned char *)文字;
    long 剩 = 长;
    while (剩 > 0) {
        int 码点;
        int 用 = 解一字(p, 剩, &码点);
        if (用 <= 0) break;
        p += 用; 剩 -= 用;

        if (码点 == '\n') {
            笔x = (double)x;
            基线 += 行高;
            continue;
        }
        if (码点 == '\r') continue;

        int 步, 左;
        stbtt_GetCodepointHMetrics(&字体, 码点, &步, &左);

        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&字体, 码点, 比, 比, &x0, &y0, &x1, &y1);
        int w = x1 - x0, h = y1 - y0;
        if (w > 0 && h > 0 && w < 4096 && h < 4096) {
            unsigned char *罩 = (unsigned char *)malloc((size_t)w * (size_t)h);
            if (罩) {
                stbtt_MakeCodepointBitmap(&字体, 罩, w, h, w, 比, 比, 码点);
                画一字(画布, (int)宽, (int)高,
                       (int)(笔x + x0 + 0.5), (int)(基线 + y0 + 0.5),
                       w, h, 罩, r, g, b, a);
                free(罩);
            }
        }
        笔x += (double)步 * 比;
        字数++;
    }
    return 字数;
}

/* 一行字有多宽（像素）。排版居中、右对齐都要先知道这个。 */
double 简_图_量(const char *文字, long long 长, double 字号)
{
    if (!备好字体() || 字号 <= 0) return 0;
    float 比 = stbtt_ScaleForPixelHeight(&字体, (float)字号);
    double 宽 = 0;
    const unsigned char *p = (const unsigned char *)文字;
    long 剩 = 长;
    while (剩 > 0) {
        int 码点;
        int 用 = 解一字(p, 剩, &码点);
        if (用 <= 0) break;
        p += 用; 剩 -= 用;
        if (码点 == '\n') continue;
        int 步, 左;
        stbtt_GetCodepointHMetrics(&字体, 码点, &步, &左);
        宽 += (double)步 * 比;
    }
    return 宽;
}

/* -------------------------------------------------------------- 版本号 -- */
/* 让 `简编译 --版本` 能报出内嵌的是哪一版 stb，出了问题好对照。 */
const char *简_图_版本(void)
{
    static char 缓[128];
    snprintf(缓, sizeof(缓), "stb_image %d / nanosvg 内置", STBI_VERSION);
    return 缓;
}
