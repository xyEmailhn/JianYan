/* ============================================================================
 *  简渲 · 建模（建模.c）
 *  ----------------------------------------------------------------------------
 *  渲染是把 3D 变成 2D；这里是反方向：把场景里的距离场变成**真正的三角网格**，
 *  导出成 .obj 和 .glb，能拖进 Blender、Unity、Three.js 继续用。
 *
 *  算法是 marching tetrahedra（行进四面体），不是 marching cubes：
 *    - cubes 要一张 256 项的大表，抄错一项就破面；
 *    - tetrahedra 只有 6 种剖分，交点连法可以现算，代码短一半，不会破面。
 *
 *  步骤：
 *      把空间切成小立方体 → 每个立方体拆 6 个四面体 →
 *      四面体四个角的内外号不同就说明表面穿过它 →
 *      在穿过的边上线性插值出交点 → 连成三角形。
 *
 *  导出的法线用距离场梯度，比三角形平均法线更准。
 * ========================================================================== */

#ifndef 简_渲_建模
#define 简_渲_建模

#include "引擎.c"

/* ------------------------------------------------------------ 包围盒 --
 */
static void 图元包围(const 图元 *p, 三 *低, 三 *高)
{
    if (!p) return;
    switch (p->类型) {
    case 元_球:
    case 元_胶囊: {
        float r = p->半径 + (p->类型 == 元_胶囊 ? p->高 : 0.0f);
        三 a = 减(p->中心, 三一(r)), b = 加(p->中心, 三一(r));
        *低 = 三造(最小(低->x, a.x), 最小(低->y, a.y), 最小(低->z, a.z));
        *高 = 三造(最大(高->x, b.x), 最大(高->y, b.y), 最大(高->z, b.z));
        break;
    }
    case 元_盒: {
        三 a = 减(p->中心, p->半), b = 加(p->中心, p->半);
        *低 = 三造(最小(低->x, a.x), 最小(低->y, a.y), 最小(低->z, a.z));
        *高 = 三造(最大(高->x, b.x), 最大(高->y, b.y), 最大(高->z, b.z));
        break;
    }
    case 元_圆柱: {
        float r = 最大(p->半径, p->高);
        三 a = 减(p->中心, 三一(r)), b = 加(p->中心, 三一(r));
        *低 = 三造(最小(低->x, a.x), 最小(低->y, a.y), 最小(低->z, a.z));
        *高 = 三造(最大(高->x, b.x), 最大(高->y, b.y), 最大(高->z, b.z));
        break;
    }
    case 元_圆环: {
        float r = p->半径 + p->高;
        三 a = 减(p->中心, 三一(r)), b = 加(p->中心, 三一(r));
        *低 = 三造(最小(低->x, a.x), 最小(低->y, a.y), 最小(低->z, a.z));
        *高 = 三造(最大(高->x, b.x), 最大(高->y, b.y), 最大(高->z, b.z));
        break;
    }
    default:
        图元包围(p->甲, 低, 高);
        图元包围(p->乙, 低, 高);
        break;
    }
}

static int 场景包围(const 场景 *s, 三 *低, 三 *高)
{
    *低 = 三一(无穷大);
    *高 = 三一(-无穷大);
    int 有 = 0;
    for (int i = 0; i < s->物数; i++) {
        /* 平面是无限的，跳过；只导实体 */
        if (s->物[i].形->类型 == 元_平面) continue;
        图元包围(s->物[i].形, 低, 高);
        有 = 1;
    }
    return 有;
}

/* ------------------------------------------------------- 行进四面体 --
 * 立方体顶点编号（和 OBJ 的右手系一致）：
 *      0(0,0,0) 1(1,0,0) 2(1,1,0) 3(0,1,0)
 *      4(0,0,1) 5(1,0,1) 6(1,1,1) 7(0,1,1)
 * 沿 0-6 对角线拆成 6 个四面体，这是标准剖分，不会产生裂缝。
 */
static const int 角点[8][3] = {
    {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
    {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};

static const int 四面体[6][4] = {
    {0, 5, 1, 6}, {0, 1, 2, 6}, {0, 2, 3, 6},
    {0, 3, 7, 6}, {0, 7, 4, 6}, {0, 4, 5, 6}};

typedef struct {
    三 *顶点;
    int 顶点数, 顶点容;
    int *索引;
    int 索引数, 索引容;
} 网格数据;

static void 合并顶点(网格数据 *m, float 精度);
static int 新顶点(网格数据 *m, 三 p);

/* 格子内顶点缓存：一个格子只有 24 条边，用局部小数组线性找比哈希还快。
   不做这一步，6 个四面体各算各的，顶点数是必要数量的三倍。 */
typedef struct {
    int 边[24];
    int 号[24];
    int 数;
} 点缓存;

static void 加顶点(网格数据 *m, 三 p)
{
    if (m->顶点数 == m->顶点容) {
        m->顶点容 = m->顶点容 ? m->顶点容 * 2 : 4096;
        m->顶点 = (三 *)realloc(m->顶点, sizeof(三) * (size_t)m->顶点容);
    }
    m->顶点[m->顶点数++] = p;
}

static int 新顶点(网格数据 *m, 三 p)
{
    加顶点(m, p);
    return m->顶点数 - 1;
}

static void 加三角形(网格数据 *m, int a, int b, int c)
{
    if (m->索引数 + 3 > m->索引容) {
        m->索引容 = m->索引容 ? m->索引容 * 2 : 8192;
        m->索引 = (int *)realloc(m->索引, sizeof(int) * (size_t)m->索引容);
    }
    m->索引[m->索引数++] = a;
    m->索引[m->索引数++] = b;
    m->索引[m->索引数++] = c;
}

/* 用一个立方体单元生成三角形 */
static void 走一个格子(网格数据 *m, 点缓存 *缓, const 场景 *s,
                      三 原点, float 边长, int gx, int gy, int gz)
{
    三 位置[8];
    float 值[8];
    for (int i = 0; i < 8; i++) {
        位置[i] = 三造(原点.x + 角点[i][0] * 边长,
                       原点.y + 角点[i][1] * 边长,
                       原点.z + 角点[i][2] * 边长);
        值[i] = 场距(s, 位置[i]);
    }

    for (int t = 0; t < 6; t++) {
        int a = 四面体[t][0], b = 四面体[t][1], c = 四面体[t][2], d = 四面体[t][3];
        int 内数 = (值[a] < 0) + (值[b] < 0) + (值[c] < 0) + (值[d] < 0);
        if (内数 == 0 || 内数 == 4) continue;

        int 号[4] = {a, b, c, d};
        int 交号[4];
        int 数 = 0;
        for (int i = 0; i < 4 && 数 < 4; i++) {
            for (int j = i + 1; j < 4 && 数 < 4; j++) {
                int p = 号[i], q = 号[j];
                if ((值[p] < 0) == (值[q] < 0)) continue;
                float 分母 = 值[p] - 值[q];
                float u = fabsf(分母) < 1e-9f ? 0.5f : 值[p] / 分母;
                u = 夹(u, 0.0f, 1.0f);
                三 pt = 混(位置[p], 位置[q], u);
                int 边 = (p < q ? p * 8 + q : q * 8 + p);
                int 号 = -1;
                for (int k = 0; k < 缓->数; k++)
                    if (缓->边[k] == 边) { 号 = 缓->号[k]; break; }
                if (号 < 0) {
                    号 = 新顶点(m, pt);
                    缓->边[缓->数] = 边;
                    缓->号[缓->数] = 号;
                    缓->数++;
                }
                交号[数++] = 号;
            }
        }
        if (数 == 3) {
            加三角形(m, 交号[0], 交号[1], 交号[2]);
        } else if (数 == 4) {
            加三角形(m, 交号[0], 交号[1], 交号[2]);
            加三角形(m, 交号[0], 交号[2], 交号[3]);
        }
    }
}

static 网格数据 提取网格(const 场景 *s0, int 精度)
{
    网格数据 m;
    memset(&m, 0, sizeof(m));

    /* 导出时把无限平面摘掉：平面把整个下半空间都算作"实体"，
       留着它，水面以下的岩石和岛底全被挖掉，模型会只剩一张薄片。 */
    场景 副本 = *s0;
    物体 *保 = (物体 *)取内存(sizeof(物体) * (size_t)(s0->物数 + 1));
    int 保数 = 0;
    for (int i = 0; i < s0->物数; i++)
        if (s0->物[i].形 && s0->物[i].形->类型 != 元_平面) 保[保数++] = s0->物[i];
    副本.物 = 保;
    副本.物数 = 保数;

    const 场景 *s = &副本;

    三 低, 高;
    if (!场景包围(s, &低, &高)) { free(保); return m; }

    /* 往外留一点边，免得表面被切在盒子边上 */
    三 边 = 减(高, 低);
    float 最大边 = 最大(边.x, 最大(边.y, 边.z));
    低 = 减(低, 三一(最大边 * 0.04f));
    高 = 加(高, 三一(最大边 * 0.04f));
    边 = 减(高, 低);

    float 步 = 最大(边.x, 最大(边.y, 边.z)) / (float)精度;
    int nx = (int)(边.x / 步) + 1, ny = (int)(边.y / 步) + 1, nz = (int)(边.z / 步) + 1;

    点缓存 缓;
    for (int z = 0; z < nz; z++)
        for (int y = 0; y < ny; y++)
            for (int x = 0; x < nx; x++) {
                缓.数 = 0;
                走一个格子(&m, &缓, s, 三造(低.x + x * 步, 低.y + y * 步, 低.z + z * 步),
                           步, x, y, z);
            }

    /* 统一朝向：三角形法线要和距离场梯度同向，否则模型在别的软件里是"里外翻的" */
    for (int i = 0; i < m.索引数; i += 3) {
        三 a = m.顶点[m.索引[i]], b = m.顶点[m.索引[i + 1]], c = m.顶点[m.索引[i + 2]];
        三 法 = 叉(减(b, a), 减(c, a));
        三 梯 = 场法线(s, 乘(加(加(a, b), c), 1.0f / 3.0f));
        if (点(法, 梯) < 0.0f) {
            int t = m.索引[i + 1];
            m.索引[i + 1] = m.索引[i + 2];
            m.索引[i + 2] = t;
        }
    }
    free(保);
    合并顶点(&m, 步 * 0.35f);
    return m;
}

/* ----------------------------------------------------------- 顶点合并 --
 * 行进四面体是逐个格子生成的，同一个交点会被相邻格子各算一遍。
 * 不合并的话顶点数是三角形的三倍，导出的文件又大、接缝又硬。
 * 这里按位置量化后查哈希表，把重合的点并成一个。
 */
static void 合并顶点(网格数据 *m, float 精度)
{
    if (m->顶点数 == 0) return;
    size_t 表长 = 1;
    while (表长 < (size_t)m->顶点数 * 2) 表长 <<= 1;
    int *表 = (int *)取内存(sizeof(int) * 表长);
    int *改 = (int *)取内存(sizeof(int) * (size_t)m->顶点数);
    三 *新点 = (三 *)取内存(sizeof(三) * (size_t)m->顶点数);
    int 新数 = 0;
    float 倒数 = 1.0f / 最大(精度, 1e-6f);

    for (int i = 0; i < m->顶点数; i++) {
        三 v = m->顶点[i];
        int kx = (int)floorf(v.x * 倒数 + 0.5f);
        int ky = (int)floorf(v.y * 倒数 + 0.5f);
        int kz = (int)floorf(v.z * 倒数 + 0.5f);
        uint32_t h = ((uint32_t)kx * 73856093u) ^ ((uint32_t)ky * 19349663u) ^ ((uint32_t)kz * 83492791u);
        size_t 位 = h & (表长 - 1);
        int 命中 = -1;
        for (;;) {
            int 槽 = 表[位];
            if (槽 == 0) break;
            int j = 槽 - 1;
            三 u = m->顶点[j];
            if (fabsf(u.x - v.x) <= 精度 && fabsf(u.y - v.y) <= 精度 && fabsf(u.z - v.z) <= 精度) {
                命中 = j;
                break;
            }
            位 = (位 + 1) & (表长 - 1);
        }
        if (命中 >= 0) {
            改[i] = 命中;
        } else {
            改[i] = 新数;
            新点[新数] = v;
            表[位] = 新数 + 1;
            新数++;
        }
    }
    for (int i = 0; i < m->索引数; i++) m->索引[i] = 改[m->索引[i]];
    free(m->顶点);
    m->顶点 = 新点;
    m->顶点数 = 新数;
    m->顶点容 = 新数;
    free(表);
    free(改);
}

/* --------------------------------------------------------------- OBJ --
 */
static int 写OBJ(const char *路径, const 场景 *s, const 网格数据 *m)
{
    char obj路径[512], mtl路径[512];
    snprintf(obj路径, sizeof(obj路径), "%s", 路径);
    snprintf(mtl路径, sizeof(mtl路径), "%s", 路径);
    char *点 = strrchr(mtl路径, '.');
    if (点) snprintf(点, sizeof(mtl路径) - (size_t)(点 - mtl路径), ".mtl");

    FILE *f = fopen(obj路径, "wb");
    if (!f) return 0;

    const char *基 = strrchr(mtl路径, '/');
    基 = 基 ? 基 + 1 : mtl路径;

    fprintf(f, "# 简渲导出的网格 · %d 顶点 · %d 三角形\n", m->顶点数, m->索引数 / 3);
    fprintf(f, "mtllib %s\n", 基);
    fprintf(f, "o 简言模型\n");

    for (int i = 0; i < m->顶点数; i++)
        fprintf(f, "v %.6f %.6f %.6f\n", m->顶点[i].x, m->顶点[i].y, m->顶点[i].z);
    for (int i = 0; i < m->顶点数; i++) {
        三 n = 场法线(s, m->顶点[i]);
        fprintf(f, "vn %.6f %.6f %.6f\n", n.x, n.y, n.z);
    }
    fprintf(f, "usemtl 简言材质\ns 1\n");
    for (int i = 0; i < m->索引数; i += 3)
        fprintf(f, "f %d//%d %d//%d %d//%d\n",
                m->索引[i] + 1, m->索引[i] + 1,
                m->索引[i + 1] + 1, m->索引[i + 1] + 1,
                m->索引[i + 2] + 1, m->索引[i + 2] + 1);
    fclose(f);

    /* 材质：取第一个物体的颜色 */
    FILE *mtl = fopen(mtl路径, "wb");
    if (mtl) {
        三 c = 三一(0.8f);
        if (s->物数 > 0) c = s->材[s->物[0].材质].基色;
        fprintf(mtl, "newmtl 简言材质\n");
        fprintf(mtl, "Kd %.4f %.4f %.4f\n", c.x, c.y, c.z);
        fprintf(mtl, "Ka 0 0 0\nKs 0.15 0.15 0.15\nNs 60\nillum 2\n");
        fclose(mtl);
    }
    return 1;
}

/* --------------------------------------------------------------- GLB --
 * glTF 2.0 二进制。三个 accessor：位置、法线、索引。
 * 手写不是为了炫技，是为了让"零依赖"这条规矩在导出这一头也成立。
 */
static void 写u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 255);
    p[1] = (unsigned char)((v >> 8) & 255);
    p[2] = (unsigned char)((v >> 16) & 255);
    p[3] = (unsigned char)((v >> 24) & 255);
}

static int 写GLB(const char *路径, const 场景 *s, const 网格数据 *m)
{
    size_t 位置字节 = (size_t)m->顶点数 * 12;
    size_t 法线字节 = (size_t)m->顶点数 * 12;
    size_t 索引字节 = (size_t)m->索引数 * 4;
    size_t 全部 = 位置字节 + 法线字节 + 索引字节;
    unsigned char *二进制 = (unsigned char *)取内存(全部 + 8);

    for (int i = 0; i < m->顶点数; i++) {
        float v[3] = {m->顶点[i].x, m->顶点[i].y, m->顶点[i].z};
        memcpy(二进制 + (size_t)i * 12, v, 12);
    }
    for (int i = 0; i < m->顶点数; i++) {
        三 n = 场法线(s, m->顶点[i]);
        float v[3] = {n.x, n.y, n.z};
        memcpy(二进制 + 位置字节 + (size_t)i * 12, v, 12);
    }
    for (int i = 0; i < m->索引数; i++)
        写u32(二进制 + 位置字节 + 法线字节 + (size_t)i * 4, (uint32_t)m->索引[i]);

    size_t 总长 = 位置字节 + 法线字节 + 索引字节;
    size_t 填充 = (4 - (总长 % 4)) % 4;
    总长 += 填充;

    三 低 = 三一(无穷大), 高 = 三一(-无穷大);
    for (int i = 0; i < m->顶点数; i++) {
        三 v = m->顶点[i];
        低 = 三造(最小(低.x, v.x), 最小(低.y, v.y), 最小(低.z, v.z));
        高 = 三造(最大(高.x, v.x), 最大(高.y, v.y), 最大(高.z, v.z));
    }

    char json[4096];
    int n = snprintf(json, sizeof(json),
        "{"
        "\"asset\":{\"version\":\"2.0\",\"generator\":\"简渲 · 简言标准库 v0.4\"},"
        "\"scene\":0,"
        "\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0,\"name\":\"简言模型\"}],"
        "\"meshes\":[{\"name\":\"简言模型\",\"primitives\":[{"
            "\"attributes\":{\"POSITION\":0,\"NORMAL\":1},"
            "\"indices\":2,\"material\":0}]}],"
        "\"materials\":[{\"name\":\"简言材质\",\"pbrMetallicRoughness\":{"
            "\"baseColorFactor\":[%.4f,%.4f,%.4f,1],\"metallicFactor\":%.3f,\"roughnessFactor\":%.3f}}],"
        "\"accessors\":["
            "{\"bufferView\":0,\"componentType\":5126,\"count\":%d,\"type\":\"VEC3\","
             "\"min\":[%.5f,%.5f,%.5f],\"max\":[%.5f,%.5f,%.5f]},"
            "{\"bufferView\":1,\"componentType\":5126,\"count\":%d,\"type\":\"VEC3\"},"
            "{\"bufferView\":2,\"componentType\":5125,\"count\":%d,\"type\":\"SCALAR\"}],"
        "\"bufferViews\":["
            "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":%zu},"
            "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
            "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}],"
        "\"buffers\":[{\"byteLength\":%zu}]"
        "}",
        0.8f, 0.8f, 0.8f, 0.15, 0.55,
        m->顶点数,
        低.x, 低.y, 低.z, 高.x, 高.y, 高.z,
        m->顶点数, m->索引数,
        位置字节,
        位置字节, 法线字节,
        位置字节 + 法线字节, 索引字节,
        总长);
    if (n <= 0 || (size_t)n >= sizeof(json)) { free(二进制); return 0; }

    size_t json长 = (size_t)n;
    size_t json填 = (4 - (json长 % 4)) % 4;
    size_t json总 = json长 + json填;

    size_t 文件长 = 12 + 8 + json总 + 8 + 总长;
    unsigned char *文件 = (unsigned char *)取内存(文件长);
    unsigned char *p = 文件;

    memcpy(p, "glTF", 4);
    写u32(p + 4, 2);
    写u32(p + 8, (uint32_t)文件长);
    p += 12;

    写u32(p, (uint32_t)json总);
    memcpy(p + 4, "JSON", 4);
    memset(p + 8, 0x20, json总);
    memcpy(p + 8, json, json长);
    p += 8 + json总;

    写u32(p, (uint32_t)总长);
    memcpy(p + 4, "BIN\0", 4);
    memcpy(p + 8, 二进制, 位置字节 + 法线字节 + 索引字节);
    memset(p + 8 + 位置字节 + 法线字节 + 索引字节, 0, 填充);

    FILE *f = fopen(路径, "wb");
    int 好 = f && fwrite(文件, 1, 文件长, f) == 文件长;
    if (f) fclose(f);

    free(文件);
    free(二进制);
    return 好;
}

#endif
