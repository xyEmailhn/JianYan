/* ============================================================================
 *  简渲 · 数学与随机（数学.h）
 *  ----------------------------------------------------------------------------
 *  渲染器里 90% 的代码在做三件事：算向量、算距离、摇骰子。
 *  这一份把这三件事做到最短——短到读一遍就不会忘。
 *
 *  为什么不用 double：一张 960×600 的图要算上亿次向量运算，
 *  float 快一倍，而 SDF 的精度只到 1e-3，double 的小数位全是浪费。
 * ========================================================================== */

#ifndef 简_渲_数学
#define 简_渲_数学

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define 派 3.14159265358979323846f
#define 无穷大 1e30f

typedef struct { float x, y, z; } 三;

static inline 三 三造(float x, float y, float z) { 三 v = {x, y, z}; return v; }
static inline 三 三零(void) { return 三造(0, 0, 0); }
static inline 三 三一(float a) { return 三造(a, a, a); }

static inline 三 加(三 a, 三 b) { return 三造(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline 三 减(三 a, 三 b) { return 三造(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline 三 乘(三 a, float s) { return 三造(a.x * s, a.y * s, a.z * s); }
static inline 三 逐乘(三 a, 三 b) { return 三造(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline float 点(三 a, 三 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline 三 叉(三 a, 三 b)
{
    return 三造(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float 长(三 a) { return sqrtf(点(a, a)); }
static inline float 长平(三 a) { return 点(a, a); }
static inline 三 归(三 a) { float l = 长(a); return l > 1e-12f ? 乘(a, 1.0f / l) : 三零(); }
static inline 三 混(三 a, 三 b, float t) { return 加(乘(a, 1.0f - t), 乘(b, t)); }
static inline float 夹(float v, float 下, float 上) { return v < 下 ? 下 : (v > 上 ? 上 : v); }
static inline float 平(float a, float b, float t) { return a + (b - a) * t; }
static inline float 最小(float a, float b) { return a < b ? a : b; }
static inline float 最大(float a, float b) { return a > b ? a : b; }
static inline 三 反射向(三 入, 三 法) { return 减(入, 乘(法, 2.0f * 点(入, 法))); }
static inline 三 按轴(三 轴, float 角)
{
    return 三造(轴.x * 角, 轴.y * 角, 轴.z * 角);
}
static inline 三 指数衰减(三 c, float k) { return 三造(c.x * k, c.y * k, c.z * k); }

/* 施密特正交：给定一个方向，造出两个和它垂直的单位向量。
   画矩形灯、建相机基、采样圆盘都要用。 */
static inline void 正交基(三 n, 三 *u, 三 *v)
{
    三 上 = fabsf(n.y) < 0.9f ? 三造(0, 1, 0) : 三造(1, 0, 0);
    *u = 归(叉(上, n));
    *v = 叉(n, *u);
}

/* ---------------------------------------------------------------- 随机数 --
 * PCG32：一个 64 位状态、两行代码、质量足够做蒙特卡洛。
 * 故意不用 rand()：它慢、质量差、还不能多线程各跑各的。
 */
typedef struct { uint64_t 态; } 随机;

static inline void 随机播种(随机 *r, uint64_t 种)
{
    r->态 = 种 * 6364136223846793005ULL + 1442695040888963407ULL;
}

static inline uint32_t 随机取(随机 *r)
{
    uint64_t 旧 = r->态;
    r->态 = 旧 * 6364136223846793005ULL + 1442695040888963407ULL;
    uint32_t x = (uint32_t)(((旧 >> 18u) ^ 旧) >> 27u);
    uint32_t 转 = (uint32_t)(旧 >> 59u);
    return (x >> 转) | (x << ((32u - 转) & 31u));
}

static inline float 随机浮(随机 *r) { return (随机取(r) >> 8) * 0x1p-24f; }
static inline float 随机范围(随机 *r, float a, float b) { return a + (b - a) * 随机浮(r); }

/* 单位球面均匀采样（Marsaglia 法，不用三角函数） */
static inline 三 随机方向(随机 *r)
{
    float z = 随机浮(r) * 2.0f - 1.0f;
    float a = 随机浮(r) * 2.0f * 派;
    float s = sqrtf(最大(0.0f, 1.0f - z * z));
    return 三造(s * cosf(a), s * sinf(a), z);
}

/* 余弦加权半球采样：漫反射弹射就该这么采，方差比"随便采再拒绝"小得多 */
static inline 三 随机余弦向(三 n, 随机 *r)
{
    三 u, v;
    正交基(n, &u, &v);
    float a = 随机浮(r) * 2.0f * 派;
    float s = sqrtf(随机浮(r));
    float t = sqrtf(1.0f - s * s);
    return 归(加(加(乘(u, cosf(a) * s), 乘(v, sinf(a) * s)), 乘(n, t)));
}

/* ---------------------------------------------------------------- 噪声 --
 * 值噪声，够做木纹、云、石材。真渲染器会用 Perlin/Simplex，
 * 但这里要的是"看起来不假"，不是"物理正确的噪声"。
 */
static inline float 哈希格(int x, int y, int z)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)z * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFFFF) / 16777215.0f;
}

static inline float 噪声(三 p)
{
    float fx = floorf(p.x), fy = floorf(p.y), fz = floorf(p.z);
    int ix = (int)fx, iy = (int)fy, iz = (int)fz;
    float tx = p.x - fx, ty = p.y - fy, tz = p.z - fz;
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    tz = tz * tz * (3 - 2 * tz);
    float c000 = 哈希格(ix, iy, iz), c100 = 哈希格(ix + 1, iy, iz);
    float c010 = 哈希格(ix, iy + 1, iz), c110 = 哈希格(ix + 1, iy + 1, iz);
    float c001 = 哈希格(ix, iy, iz + 1), c101 = 哈希格(ix + 1, iy, iz + 1);
    float c011 = 哈希格(ix, iy + 1, iz + 1), c111 = 哈希格(ix + 1, iy + 1, iz + 1);
    float x00 = 平(c000, c100, tx), x10 = 平(c010, c110, tx);
    float x01 = 平(c001, c101, tx), x11 = 平(c011, c111, tx);
    return 平(平(x00, x10, ty), 平(x01, x11, ty), tz);
}

/* 分形噪声：三层叠加，做出"大块带细节"的自然感 */
static inline float 分形噪声(三 p)
{
    return 噪声(p) * 0.6f + 噪声(乘(p, 2.03f)) * 0.28f + 噪声(乘(p, 4.11f)) * 0.12f;
}

#endif
