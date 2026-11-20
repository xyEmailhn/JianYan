/* ============================================================================
 *  简渲 · 主程序（简渲.c）
 *  ----------------------------------------------------------------------------
 *  用法：
 *      简渲 场景.scene                     出图，文件名写在场景里
 *      简渲 场景.scene -o 成品.png
 *      简渲 场景.scene -s 256 -w 1280 -h 800
 *      简渲 场景.scene -预览                320 宽、16 采样，先看构图
 *      简渲 场景.scene -线程 16
 *      简渲                                （不带参数）跑内置的三球体检场景
 *
 *  这份是对「简言标准库 v0.4」的**参考实现**：同样的参数名、同样的默认值。
 *  等后端做完，`渲染.快照(场景, 选项)` 编译出来就是这一份代码。
 * ========================================================================== */

#include <time.h>
#include <unistd.h>

#include "着色.c"
#include "图像.c"
#include "场景.c"
#include "建模.c"

/* ------------------------------------------------------------ 自动取景 --
 * 场景里没写相机时，按所有物体的包围盒自动摆机位。
 * 这条规则很关键：AI 只写"球、盒、光"，不该还得算相机距离。
 */
static void 自动相机(场景 *s)
{
    三 低 = 三一(无穷大), 高 = 三一(-无穷大);
    int 有 = 0;
    for (int i = 0; i < s->物数; i++) {
        图元包围(s->物[i].形, &低, &高);
        有 = 1;
    }
    if (!有) return;
    三 中 = 乘(加(低, 高), 0.5f);
    float 半 = 最大(0.25f, 长(减(高, 低)) * 0.5f);
    float 距 = 半 * 2.1f / tanf(s->机.视场 * 0.5f * 派 / 180.0f);
    s->机.目标 = 中;
    s->机.位置 = 加(中, 三造(0.0f, 半 * 0.35f, 距));
    s->机.上 = 三造(0, 1, 0);
}

/* ---------------------------------------------------------------- 说明 --
 */
static void 用法(void)
{
    printf(
        "简渲 · 简言标准库 v0.4 渲染参考实现\n"
        "\n"
        "  简渲 场景.scene [选项]\n"
        "  简渲                       跑内置体检场景\n"
        "\n"
        "  -o 文件.png    输出路径\n"
        "  -w 宽 -h 高    分辨率\n"
        "  -s 采样        每像素样本数（越大越干净，越慢）\n"
        "  -线程 数       worker 线程数，默认用满所有核\n"
        "  -预览          320 宽 + 16 采样，先看构图\n"
        "  -不泛光        关掉 bloom\n"
        "  -统计          只打印场景摘要，不渲染\n"
        "  -模型 名字     导出 .obj 和 .glb，不渲染（默认精度 96）\n"
        "\n"
        "场景文件是「空格版函数调用」：\n"
        "  球 (0, 0.8, 0) 0.8 材质 金\n"
        "  光 面 (3, 4, 2) 大小 (3, 2) 强度 8\n"
        "  相机 位置 (0, 1.4, 5) 看向 (0, 0.8, 0) 视场 34\n");
}

static double 现在(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    场景 s;
    memset(&s, 0, sizeof(s));
    s.宽 = 0;
    s.高 = 0;
    s.采样 = 64;
    s.曝光 = 1.0f;
    s.泛光 = 0.05f;
    s.暗角 = 0.20f;
    s.颗粒 = 0.006f;
    s.环境强度 = 1.0f;
    s.机.视场 = 38.0f;

    装内置材质(&s);

    const char *场景路径 = NULL;
    const char *出图 = NULL;
    const char *模型路径 = NULL;
    int 预览 = 0, 只统计 = 0, 有相机 = 0;
    int 采样覆盖 = 0, 宽覆盖 = 0, 高覆盖 = 0;
    int 覆盖宽 = 0, 覆盖高 = 0, 覆盖采样 = 0;

    for (int i = 1; i < argc; i++) {
        if (同(argv[i], "-o")) { if (++i < argc) 出图 = argv[i]; }
        else if (同(argv[i], "-w")) { if (++i < argc) { 覆盖宽 = atoi(argv[i]); 宽覆盖 = 1; } }
        else if (同(argv[i], "-h")) { if (++i < argc) { 覆盖高 = atoi(argv[i]); 高覆盖 = 1; } }
        else if (同(argv[i], "-s")) { if (++i < argc) { 覆盖采样 = atoi(argv[i]); 采样覆盖 = 1; } }
        else if (同(argv[i], "-线程")) { if (++i < argc) setenv("简渲线程", argv[i], 1); }
        else if (同(argv[i], "-预览")) 预览 = 1;
        else if (同(argv[i], "-不泛光")) s.泛光 = 0.0f;
        else if (同(argv[i], "-统计")) 只统计 = 1;
        else if (同(argv[i], "-模型")) { if (++i < argc) 模型路径 = argv[i]; }
        else if (同(argv[i], "--help") || 同(argv[i], "-帮助")) { 用法(); return 0; }
        else if (argv[i][0] != '-') 场景路径 = argv[i];
    }

    double 起 = 现在();

    if (场景路径) {
        if (!读场景(&s, 场景路径)) {
            fprintf(stderr, "打不开场景文件：%s\n", 场景路径);
            return 1;
        }
    } else {
        默认场景(&s);
    }

    if (!s.相机已设) 自动相机(&s);
    (void)有相机;

    /* 命令行 > 场景文件 > 默认值 */
    if (宽覆盖) s.宽 = 覆盖宽;
    if (高覆盖) s.高 = 覆盖高;
    if (采样覆盖) s.采样 = 覆盖采样;

    if (预览) {
        s.宽 = 320;
        s.高 = 200;
        if (!采样覆盖) s.采样 = 16;
    } else {
        if (s.宽 <= 0) s.宽 = 900;
        if (s.高 <= 0) s.高 = 560;
    }
    if (s.宽 < 16) s.宽 = 16;
    if (s.高 < 16) s.高 = 16;
    if (s.采样 < 1) s.采样 = 1;
    if (s.采样 > 4096) s.采样 = 4096;

    size_t 像素数 = (size_t)s.宽 * (size_t)s.高;
    float *缓 = (float *)取内存(sizeof(float) * 像素数 * 3);

    if (模型路径 || s.导出网格) {
        const char *基 = 模型路径 ? 模型路径 : "模型";
        int 精度 = s.网格精度 > 0 ? (int)s.网格精度 : 96;
        网格数据 m = 提取网格(&s, 精度);
        if (m.索引数 == 0) {
            fprintf(stderr, "场景里没有可导出的实体（全是平面？）\n");
            return 1;
        }
        char obj路径[512], glb路径[512];
        char *点 = strrchr(基, '.');
        size_t 主干 = 点 ? (size_t)(点 - 基) : strlen(基);
        snprintf(obj路径, sizeof(obj路径), "%.*s.obj", (int)主干, 基);
        snprintf(glb路径, sizeof(glb路径), "%.*s.glb", (int)主干, 基);
        写OBJ(obj路径, &s, &m);
        写GLB(glb路径, &s, &m);
        printf("导出 %d 顶点 / %d 三角形 → %s、%s\n", m.顶点数, m.索引数 / 3, obj路径, glb路径);
        free(m.顶点);
        free(m.索引);
        return 0;
    }

    if (只统计) {
        printf("场景：%d 个物体，%d 个材质，%d 个光源\n", s.物数, s.材数, s.光数);
        printf("输出：%d × %d，%d 采样\n", s.宽, s.高, s.采样);
        for (int i = 0; i < s.物数; i++) {
            材质 *m = &s.材[s.物[i].材质];
            printf("  物体 %-8s 材质 %-10s 基色(%.2f %.2f %.2f) 粗%.2f 金%.2f\n",
                   s.物[i].名, m->名, m->基色.x, m->基色.y, m->基色.z, m->粗糙, m->金属);
        }
        return 0;
    }

    printf("简渲 · %d×%d · %d 采样 · %.1f 秒内出图\n",
           s.宽, s.高, s.采样, 0.0);
    fflush(stdout);

    渲染(&s, 缓);
    double 渲完 = 现在();

    unsigned char *图 = 成像(&s, 缓);
    double 成像完 = 现在();

    char 路径[512];
    if (出图) snprintf(路径, sizeof(路径), "%s", 出图);
    else if (s.输出[0]) snprintf(路径, sizeof(路径), "%s", s.输出);
    else snprintf(路径, sizeof(路径), "渲染.png");

    if (!写PNG(路径, 图, s.宽, s.高)) {
        fprintf(stderr, "写不了 %s\n", 路径);
        return 1;
    }
    double 完 = 现在();

    printf("渲染 %.2f 秒 · 成像 %.2f 秒 · 总共 %.2f 秒\n", 渲完 - 起, 成像完 - 渲完, 完 - 起);
    printf("出图：%s（%d×%d，%d 采样，%d 物体）\n", 路径, s.宽, s.高, s.采样, s.物数);

    free(缓);
    free(图);
    return 0;
}
