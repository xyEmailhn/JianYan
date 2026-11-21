/* ============================================================================
 *  简言 · 后端（C 参考后端）
 *  ----------------------------------------------------------------------------
 *  输入：IR（三地址码 + 基本块）
 *  输出：一份 C 源文件，交给 gcc 变成机器码
 *
 *  为什么先出 C 而不是直接出机器码？
 *
 *  到这一步，前面四棒（词法/语法/语义/IR）加起来一万多行，但从没有一个
 *  .jy 文件真正被执行过。所有「正确」都是纸面上的。
 *
 *  如果现在直接写 x86-64，第一次跑崩了，你分不清是 IR 生成错了、还是
 *  指令选错了、还是寄存器分配错了。三层 bug 混在一起，调一个下午可能
 *  只挪动一行。
 *
 *  先出 C 有四个好处：
 *    1. gcc 替我们做指令选择和寄存器分配，这两块最难的部分先不碰
 *    2. 生成的 C 能读，IR 错了肉眼就能看出来
 *    3. 立刻能跑真实程序，把 IR 的 bug 全逼出来
 *    4. 后端结构定下来之后，换成 x86-64 只是把「写 C 语句」换成「发机器码」
 *
 *  这个后端叫「参考后端」：它是后端的规格说明书，也是原生后端的对照组。
 *  两边跑同一批测试，输出必须完全一致。
 *
 *  ---- 映射规则 ----
 *
 *  每个 IR 值       → 一个 C 局部变量 v<N>
 *  每个基本块       → 一个 C 标签 L<N>，块的先后顺序不重要，全靠 goto
 *  变量槽（%甲）    → 也是个普通 C 变量，载/存 就是赋值
 *
 *  这套映射笨到不能再笨：值不复用、变量不合并、块不重排。gcc -O2 会把
 *  这些全优化掉（它做的正是寄存器分配和活跃变量分析）。我们省下的每
 *  一行，都是原生后端将来要补的一行。
 * ========================================================================== */

#define 简_只要中间 1
#include "../中间/ir.c"

#include <stdint.h>
#include <unistd.h>

/* ------------------------------------------------------------ 输出缓冲 -- */

static FILE *出 = NULL;

static void 行(const char *格式, ...)
{
    va_list 参;
    va_start(参, 格式);
    vfprintf(出, 格式, 参);
    va_end(参);
    fputc('\n', 出);
}

/* -------------------------------------------------------------- 类型 -- */

/* 简言类型 → C 类型。
 * 先只分三类：浮点、字符串、其余。枚举/结构/组这些后面按需细化。 */
static char 型缓[2][160];
static int 型缓轮 = 0;

static const char *C型(类型 *t)
{
    if (!t) return "long long";
    switch (t->种) {
    case 型_浮点:   return "double";
    case 型_字符串: return "简字";
    case 型_图:     return "简图";
    case 型_大整数: return "简大整";
    case 型_小数:   return "简小数";
    case 型_声音:   return "简声";
    case 型_组:     return "简组";
    case 型_映射:   return "简映";
    case 型_结果:   return "简果";
    case 型_通道:   return "简道";
    case 型_空:     return "void";
    case 型_布尔:   return "long long";
    case 型_可空:   return C型(t->元素);
    case 型_命名: {
        /* 名字不是 NUL 结尾，抄一份出来。两轮缓冲，够同时写两个类型了。 */
        char *b = 型缓[型缓轮];
        型缓轮 ^= 1;
        int n = t->名长 < 150 ? t->名长 : 150;
        memcpy(b, t->名 ? t->名 : "?", (size_t)n);
        b[n] = 0;
        return b;
    }
    default:        return "long long";
    }
}

/* 结构/枚举定义：从 AST 里取，生成 C 的 typedef。
 *
 * 结构   →  typedef struct { T 字段; ... } 名;
 * 枚举   →  带标签的联合：
 *            typedef struct { int 变体; union { struct {...} 圆; ... } 值; } 形状;
 *
 * 为什么不用「每个变体一个类型 + 指针」？因为简言枚举是值类型，
 * 递归枚举的规定也是「通过容器间接」。用标签联合，拷贝就是 memcpy，
 * 没有所有权问题。
 */
/* 结构体字段名册：构造的时候要用它写指定初始化器
 *   (点){ .x = v0, .y = v1 }
 * 而不是按位置排：
 *   (点){ v0, v1 }
 * 位置式一旦中间漏一个字段，后面全部错位；指定式只认名字，缺的 C 自己清零。
 * 顺带一提，指定式生成的代码也更好读——出问题的时候能一眼看出哪一项不对。 */
typedef struct {
    const char *名;
    int 名长;
    const char *字段[16];
    int 字段长[16];
    int 数;
} 字段册;

static 字段册 字段册表[64];
static int 字段册数 = 0;

static void 记字段册(节点 *n)
{
    if (字段册数 >= 64) return;
    字段册 *册 = &字段册表[字段册数++];
    册->名 = n->文字;
    册->名长 = n->文字长;
    for (int k = 0; k < n->子数 && 册->数 < 16; k++) {
        if (n->子[k]->种类 != 节_字段) continue;
        册->字段[册->数] = n->子[k]->文字;
        册->字段长[册->数] = n->子[k]->文字长;
        册->数++;
    }
}

static 字段册 *找字段册(const char *名, int 名长)
{
    for (int i = 0; i < 字段册数; i++)
        if (名同(字段册表[i].名, 字段册表[i].名长, 名, 名长)) return &字段册表[i];
    return NULL;
}

static void 写类型定义(节点 *根)
{
    int 有 = 0;

    for (int i = 0; i < 根->子数; i++) {
        节点 *n = 根->子[i];
        if (n->种类 != 节_结构 && n->种类 != 节_枚举) continue;

        if (!有) {
            行("/* ---- 用户定义的类型 ---- */");
            有 = 1;
        }

        if (n->种类 == 节_结构) {
            记字段册(n);
            fprintf(出, "typedef struct {\n");
            int 无字段 = 1;
            for (int k = 0; k < n->子数; k++) {
                节点 *f = n->子[k];
                if (f->种类 != 节_字段) continue;
                类型 *ft = NULL;
                for (int j = 0; j < f->子数; j++)
                    if (f->子[j]->种类 == 节_类型) ft = 节点类型(f->子[j]);
                fprintf(出, "    %s %.*s;\n", C型(ft), f->文字长, f->文字);
                无字段 = 0;
            }
            if (无字段) fputs("    char 空;\n", 出);
            fprintf(出, "} %.*s;\n", n->文字长, n->文字);
        } else {
            /* 先数一下字段最多的变体不需要特别处理，union 自动取最大 */
            fprintf(出, "enum { ");
            int 首 = 1;
            for (int k = 0; k < n->子数; k++) {
                节点 *v = n->子[k];
                if (v->种类 != 节_变体) continue;
                if (!首) fputs(", ", 出);
                fprintf(出, "%.*s_%.*s = %d", n->文字长, n->文字, v->文字长, v->文字, k);
                首 = 0;
            }
            if (首) fprintf(出, "%.*s_无 = 0", n->文字长, n->文字);
            fprintf(出, " };\n");

            fprintf(出, "typedef struct { int 变体; union {\n");
            for (int k = 0; k < n->子数; k++) {
                节点 *v = n->子[k];
                if (v->种类 != 节_变体) continue;
                fprintf(出, "    struct { ");
                int 无字段 = 1;
                for (int j = 0; j < v->子数; j++) {
                    节点 *f = v->子[j];
                    if (f->种类 != 节_字段) continue;
                    类型 *ft = NULL;
                    for (int m = 0; m < f->子数; m++)
                        if (f->子[m]->种类 == 节_类型) ft = 节点类型(f->子[m]);
                    fprintf(出, "%s %.*s; ", C型(ft), f->文字长, f->文字);
                    无字段 = 0;
                }
                if (无字段) fputs("char 空;", 出);
                fprintf(出, "} %.*s;\n", v->文字长, v->文字);
            }
            fprintf(出, "} 值; } %.*s;\n", n->文字长, n->文字);
        }
        fputc('\n', 出);
    }
}

/* ---------------------------------------------------------- 名字修饰 -- */

/* IR 函数名可能带 $（单态化实例，如 最大$整数）。$ 在 C 里不是标准字符，
 * 虽然 gcc 允许，但不值得赌。换成 _。 */
static void 写函数名(const char *名, int 长)
{
    for (int i = 0; i < 长; i++) {
        unsigned char c = (unsigned char)名[i];
        if (c == '$') fputc('_', 出);
        else fputc(c, 出);
    }
}

/* 借用参数的 C 表示：函数签名里是 `T *v3`，函数体里所有用到 v3 的地方
 * 都要解引用。`可变借用` 靠这一条才真的能改到调用方——
 * 少了它，`平移(p, dx, dy)` 改的是副本，调用方一点感觉都没有。 */
static 中间函数 *当前函 = NULL;
static unsigned char *当前指针参 = NULL;
static int 当前值数 = 0;

static void 值名(int 号)
{
    if (当前指针参 && 号 >= 0 && 号 < 当前值数 && 当前指针参[号])
        fprintf(出, "(*v%d)", 号);
    else
        fprintf(出, "v%d", 号);
}

/* 实参传指针时要写 &：`&v3` */
static void 取址名(int 号)
{
    if (当前指针参 && 号 >= 0 && 号 < 当前值数 && 当前指针参[号])
        fprintf(出, "v%d", 号);   /* 已经是指针，直接给 */
    else
        fprintf(出, "&v%d", 号);
}

/* -------------------------------------------------------- C 字符串转义 -- */

/* 返回剥掉引号之后的实际字节数。
 *
 * 要分两步：先把源文件里的转义（\n、\"、\\）还原成真正的字节，
 * 再按 C 的规则重新转义。直接照抄会把 \" 变成一个反斜杠加一个引号——
 * 编译能过，但打印出来会多一层反斜杠。 */
static int 写字面量(const char *s, int n)
{
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') { s++; n -= 2; }

    char *解 = (char *)malloc((size_t)n + 1);
    int 长 = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            char e = s[++i];
            switch (e) {
            case 'n': 解[长++] = '\n'; break;
            case 't': 解[长++] = '\t'; break;
            case 'r': 解[长++] = '\r'; break;
            case '0': 解[长++] = '\0'; break;
            case '"': 解[长++] = '"';  break;
            case '\\': 解[长++] = '\\'; break;
            default:  解[长++] = e;    break;
            }
        } else {
            解[长++] = s[i];
        }
    }

    fputc('"', 出);
    for (int i = 0; i < 长; i++) {
        unsigned char c = (unsigned char)解[i];
        switch (c) {
        case '"':  fputs("\\\"", 出); break;
        case '\\': fputs("\\\\", 出); break;
        case '\n': fputs("\\n", 出);  break;
        case '\r': fputs("\\r", 出);  break;
        case '\t': fputs("\\t", 出);  break;
        default:
            if (c < 0x20) fprintf(出, "\\%03o", c);
            else fputc(c, 出);
        }
    }
    fputc('"', 出);
    free(解);
    return 长;
}

/* ------------------------------------------------------------ 运行时 -- */

/* 这里的内嵌运行时只放「语言语义必须有、但 C 没有」的东西：
 * 带长度的字符串（简言字符串是 UTF-8 字节串 + 显式长度，不是 C 的 NUL 结尾）、
 * 乘方、打印分派、整数转字符串。
 *
 * 它会被原样拼进生成的 .c 文件，所以生成结果永远自包含。 */
static void 写运行时(void)
{
    行("/* ---- 简言运行时（内嵌，随生成文件一起走） ---- */");
    行("#include <stdio.h>");
    行("#include <stdlib.h>");
    行("#include <string.h>");
    行("#include <math.h>");
    行("#include <stdint.h>");
    行("");
    行("typedef struct { const char *p; int64_t n; } 简字;");
    行("");
    行("/* 组：一段连续内存 + 元素个数 + 元素宽度。");
    行("   不做泛型容器类型，用运行时的字节宽度区分元素，");
    行("   生成代码按元素类型强转指针访问——类型安全靠编译期检查保证。 */");
    行("typedef struct { char *p; int64_t n, 容; int 元宽; } 简组;");
    行("typedef struct { 简组 g; int64_t i; } 简遍;");
    行("static 简组 简_组(void *源, int64_t n, int 元宽) {");
    行("    简组 g; g.n = n; g.容 = n; g.元宽 = 元宽;");
    行("    g.p = (char *)malloc((size_t)(n * 元宽 + 1));");
    行("    if (!g.p) { fputs(\"内存不够了\\n\", stderr); exit(3); }");
    行("    if (n) memcpy(g.p, 源, (size_t)(n * 元宽));");
    行("    return g;");
    行("}");
    行("static void 简_越界(int64_t i, int64_t n) {");
    /* 注意 `%%lld`：行() 本身是 vfprintf，单个 % 会被当成它自己的格式符，
     * 于是生成的 C 里那行 fprintf 就丢了占位符，越界时报出来的是两个垃圾数。
     * 这个 bug 藏了很久——因为只有真越界时才会走到这里。 */
    行("    fprintf(stderr, \"下标越界：%%lld，长度 %%lld\\n\", (long long)i, (long long)n);");
    行("    exit(6);");
    行("}");
    行("");
    行("static 简字 简_字(const char *p, int64_t n) { 简字 s; s.p = p; s.n = n; return s; }");
    行("static 简字 简_拼(简字 a, 简字 b) {");
    行("    char *p = (char *)malloc((size_t)(a.n + b.n + 1));");
    行("    if (!p) { fputs(\"内存不够了\\n\", stderr); exit(3); }");
    行("    if (a.n) memcpy(p, a.p, (size_t)a.n);");
    行("    if (b.n) memcpy(p + a.n, b.p, (size_t)b.n);");
    行("    p[a.n + b.n] = 0;");
    行("    return 简_字(p, a.n + b.n);");
    行("}");
    行("static int 简_比较(简字 a, 简字 b) {");
    行("    int64_t n = a.n < b.n ? a.n : b.n;");
    行("    int c = n ? memcmp(a.p, b.p, (size_t)n) : 0;");
    行("    if (c) return c;");
    行("    return a.n < b.n ? -1 : (a.n > b.n ? 1 : 0);");
    行("}");
    行("static 简字 简_整字(long long v) {");
    行("    char b[32]; int n = snprintf(b, sizeof(b), \"%%lld\", v);");
    行("    char *p = (char *)malloc((size_t)n + 1);");
    行("    memcpy(p, b, (size_t)n); p[n] = 0;");
    行("    return 简_字(p, n);");
    行("}");
    行("static 简字 简_浮字(double v) {");
    行("    char b[64]; int n = snprintf(b, sizeof(b), \"%%g\", v);");
    行("    char *p = (char *)malloc((size_t)n + 1);");
    行("    memcpy(p, b, (size_t)n); p[n] = 0;");
    行("    return 简_字(p, n);");
    行("}");
    行("static long long 简_幂(long long b, long long e) {");
    行("    if (e < 0) return 0;");
    行("    long long r = 1;");
    行("    while (e > 0) { if (e & 1) r *= b; b *= b; e >>= 1; }");
    行("    return r;");
    行("}");
    行("static void 简_打印(简字 s) { if (s.n) fwrite(s.p, 1, (size_t)s.n, stdout); }");
    行("");
    行("/* ---- 组的增删查 ---- */");
    行("static void 简_留够(简组 *g, int64_t 需) {");
    行("    if (需 <= g->容) return;");
    行("    int64_t 新 = g->容 ? g->容 : 8;");
    行("    while (新 < 需) 新 *= 2;");
    行("    g->p = (char *)realloc(g->p, (size_t)(新 * g->元宽 + 1));");
    行("    if (!g->p) { fputs(\"内存不够了\\n\", stderr); exit(3); }");
    行("    g->容 = 新;");
    行("}");
    行("static void 简_追加(简组 *g, const void *元) {");
    行("    简_留够(g, g->n + 1);");
    行("    memcpy(g->p + g->n * g->元宽, 元, (size_t)g->元宽);");
    行("    g->n++;");
    行("}");
    行("static void 简_插入(简组 *g, int64_t i, const void *元) {");
    行("    if (i < 0) i = 0; if (i > g->n) i = g->n;");
    行("    简_留够(g, g->n + 1);");
    行("    memmove(g->p + (i + 1) * g->元宽, g->p + i * g->元宽,");
    行("            (size_t)((g->n - i) * g->元宽));");
    行("    memcpy(g->p + i * g->元宽, 元, (size_t)g->元宽);");
    行("    g->n++;");
    行("}");
    行("static int 简_含(简组 *g, const void *元) {");
    行("    for (int64_t i = 0; i < g->n; i++)");
    行("        if (memcmp(g->p + i * g->元宽, 元, (size_t)g->元宽) == 0) return 1;");
    行("    return 0;");
    行("}");
    行("static 简组 简_复制组(简组 g) { return 简_组(g.p, g.n, g.元宽); }");
    行("static void 简_清空(简组 *g) { g->n = 0; }");
    行("static int 简_排类;");
    行("static int 简_比元(const void *a, const void *b) {");
    行("    if (简_排类 == 1) { long long x = *(const long long *)a, y = *(const long long *)b; return x < y ? -1 : x > y; }");
    行("    if (简_排类 == 2) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }");
    行("    return 简_比较(*(const 简字 *)a, *(const 简字 *)b);");
    行("}");
    行("static void 简_排序(简组 *g, int 类) { 简_排类 = 类; qsort(g->p, (size_t)g->n, (size_t)g->元宽, 简_比元); }");
    行("");
    行("/* ---- 映射：有序数组 + 线性查找 ----");
    行("   不是哈希表。语言层面按键取值是 O(n)，但 n 通常很小，");
    行("   省掉哈希函数、扩容、冲突链三块代码，生成程序也小得多。");
    行("   真需要 O(1) 时换成哈希表，接口不变。 */");
    行("typedef struct { char *p; int64_t n, 容; int 键宽, 值宽, 键类; } 简映;");
    行("static int 简_键比(简映 *m, const void *a, const void *b) {");
    行("    if (m->键类 == 1) { long long x = *(const long long *)a, y = *(const long long *)b; return x < y ? -1 : x > y; }");
    行("    if (m->键类 == 2) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }");
    行("    return 简_比较(*(const 简字 *)a, *(const 简字 *)b);");
    行("}");
    行("static int64_t 简_映找(简映 *m, const void *k) {");
    行("    for (int64_t i = 0; i < m->n; i++)");
    行("        if (简_键比(m, m->p + i * (m->键宽 + m->值宽), k) == 0) return i;");
    行("    return -1;");
    行("}");
    行("static void 简_映留(简映 *m, int64_t 需) {");
    行("    if (需 <= m->容) return;");
    行("    int64_t 新 = m->容 ? m->容 : 8;");
    行("    while (新 < 需) 新 *= 2;");
    行("    m->p = (char *)realloc(m->p, (size_t)(新 * (m->键宽 + m->值宽) + 1));");
    行("    if (!m->p) { fputs(\"内存不够了\\n\", stderr); exit(3); }");
    行("    m->容 = 新;");
    行("}");
    行("static int 简_映取(简映 *m, const void *k, void *出) {");
    行("    int64_t i = 简_映找(m, k);");
    行("    if (i < 0) { memset(出, 0, (size_t)m->值宽); return 0; }");
    行("    memcpy(出, m->p + i * (m->键宽 + m->值宽) + m->键宽, (size_t)m->值宽);");
    行("    return 1;");
    行("}");
    行("static int 简_映插(简映 *m, const void *k, const void *v, void *旧) {");
    行("    int64_t i = 简_映找(m, k);");
    行("    if (i >= 0) {");
    行("        char *槽 = m->p + i * (m->键宽 + m->值宽) + m->键宽;");
    行("        memcpy(旧, 槽, (size_t)m->值宽);");
    行("        memcpy(槽, v, (size_t)m->值宽);");
    行("        return 1;");
    行("    }");
    行("    简_映留(m, m->n + 1);");
    行("    char *槽 = m->p + m->n * (m->键宽 + m->值宽);");
    行("    memcpy(槽, k, (size_t)m->键宽);");
    行("    memcpy(槽 + m->键宽, v, (size_t)m->值宽);");
    行("    m->n++;");
    行("    memset(旧, 0, (size_t)m->值宽);");
    行("    return 0;");
    行("}");
    行("static int 简_映含(简映 *m, const void *k) { return 简_映找(m, k) >= 0; }");
    行("static int 简_映删(简映 *m, const void *k, void *旧) {");
    行("    int64_t i = 简_映找(m, k);");
    行("    if (i < 0) { memset(旧, 0, (size_t)m->值宽); return 0; }");
    行("    char *槽 = m->p + i * (m->键宽 + m->值宽);");
    行("    memcpy(旧, 槽 + m->键宽, (size_t)m->值宽);");
    行("    memmove(槽, 槽 + m->键宽 + m->值宽,");
    行("            (size_t)((m->n - i - 1) * (m->键宽 + m->值宽)));");
    行("    m->n--;");
    行("    return 1;");
    行("}");
    行("static 简组 简_映键(简映 *m) {");
    行("    简组 g; g.n = m->n; g.容 = m->n; g.元宽 = m->键宽;");
    行("    g.p = (char *)malloc((size_t)(m->n * m->键宽 + 1));");
    行("    for (int64_t i = 0; i < m->n; i++)");
    行("        memcpy(g.p + i * m->键宽, m->p + i * (m->键宽 + m->值宽), (size_t)m->键宽);");
    行("    return g;");
    行("}");
    行("static 简组 简_映值(简映 *m) {");
    行("    简组 g; g.n = m->n; g.容 = m->n; g.元宽 = m->值宽;");
    行("    g.p = (char *)malloc((size_t)(m->n * m->值宽 + 1));");
    行("    for (int64_t i = 0; i < m->n; i++)");
    行("        memcpy(g.p + i * m->值宽, m->p + i * (m->键宽 + m->值宽) + m->键宽, (size_t)m->值宽);");
    行("    return g;");
    行("}");
    行("");
    行("/* ---- 结果：成功/失败 + 堆上的载荷 ----");
    行("   载荷用 void* 加宽度存，是因为 成功(学生) 和 失败(字符串) 的");
    行("   载荷类型不同，C 里没法用同一个静态类型装下两者。");
    行("   取用时按目标类型 sizeof 拷回来，编译器保证类型对得上。 */");
    行("typedef struct { int 成; char *p; int 宽; } 简果;");
    行("/* 结果 的载荷缓冲固定 128 字节，不按实际宽度分配。");
    行("   原因：键和值可能是不同的类型（结果[图, 字符串]，一个 24 字节一个 16 字节），");
    行("   构造时只知道存的这一头有多宽，取值时却按另一头拷。");
    行("   按实际宽度 malloc 的话，取值那一侧会读到堆上的相邻字节——");
    行("   整数结果偶尔是个随机数，查起来极难。多占 128 字节买个安心。 */");
    行("static 简果 简_果造(int 成, const void *v, int 宽) {");
    行("    简果 g; g.成 = 成;");
    行("    if (宽 < 0) 宽 = 0; if (宽 > 128) 宽 = 128;");
    行("    g.宽 = 宽;");
    行("    g.p = (char *)calloc(128, 1);");
    行("    if (!g.p) { fputs(\"内存不够了\\n\", stderr); exit(3); }");
    行("    if (宽 > 0 && v) memcpy(g.p, v, (size_t)宽);");
    行("    return g;");
    行("}");
    行("");
    行("/* ---- 通道 ----");
    行("   环形缓冲。注意：IR 目前把 `线程 { }` 块内联成顺序执行，");
    行("   所以这里不需要锁。等真并发了，加 mutex + cond 即可，接口不变。 */");
    行("typedef struct { char *p; int64_t n, 容, 头; int 宽; int 关; } 简道;");
    行("static 简道 简_通道(int64_t 容, int 宽) {");
    行("    简道 c; c.宽 = 宽; c.n = 0; c.头 = 0; c.关 = 0;");
    行("    c.容 = 容 > 0 ? 容 : 1;");
    行("    c.p = (char *)malloc((size_t)(c.容 * 宽 + 1));");
    行("    if (!c.p) { fputs(\"内存不够了\\n\", stderr); exit(3); }");
    行("    return c;");
    行("}");
    行("static void 简_发(简道 *c, const void *v) {");
    行("    if (c->n >= c->容) { fputs(\"通道满了\\n\", stderr); return; }");
    行("    memcpy(c->p + ((c->头 + c->n) %% c->容) * c->宽, v, (size_t)c->宽);");
    行("    c->n++;");
    行("}");
    行("static int 简_收(简道 *c, void *出) {");
    行("    if (c->n <= 0) { memset(出, 0, (size_t)c->宽); return 0; }");
    行("    memcpy(出, c->p + c->头 * c->宽, (size_t)c->宽);");
    行("    c->头 = (c->头 + 1) %% c->容;");
    行("    c->n--;");
    行("    return 1;");
    行("}");
    行("");
    行("/* ---- 字符串 ---- */");
    行("static int64_t 简_字数(简字 s) {");
    行("    int64_t n = 0;");
    行("    for (int64_t i = 0; i < s.n; i++) if ((s.p[i] & 0xC0) != 0x80) n++;");
    行("    return n;");
    行("}");
    行("static int64_t 简_字节数(简字 s) { return s.n; }");
    行("static int 简_字含(简字 a, 简字 b) {");
    行("    if (b.n <= 0) return 1;");
    行("    if (b.n > a.n) return 0;");
    行("    for (int64_t i = 0; i + b.n <= a.n; i++)");
    行("        if (memcmp(a.p + i, b.p, (size_t)b.n) == 0) return 1;");
    行("    return 0;");
    行("}");
    行("static 简字 简_大小写(简字 s, int 上) {");
    行("    char *p = (char *)malloc((size_t)s.n + 1);");
    行("    for (int64_t i = 0; i < s.n; i++) {");
    行("        char c = s.p[i];");
    行("        if (上 && c >= 'a' && c <= 'z') c = (char)(c - 32);");
    行("        if (!上 && c >= 'A' && c <= 'Z') c = (char)(c + 32);");
    行("        p[i] = c;");
    行("    }");
    行("    p[s.n] = 0;");
    行("    return 简_字(p, s.n);");
    行("}");
    行("static 简字 简_去空白(简字 s) {");
    行("    int64_t a = 0, b = s.n;");
    行("    while (a < b && (s.p[a] == ' ' || s.p[a] == '\\t' || s.p[a] == '\\n' || s.p[a] == '\\r')) a++;");
    行("    while (b > a && (s.p[b-1] == ' ' || s.p[b-1] == '\\t' || s.p[b-1] == '\\n' || s.p[b-1] == '\\r')) b--;");
    行("    return 简_字(s.p + a, b - a);");
    行("}");
    行("static int 简_开头(简字 a, 简字 b) {");
    行("    return b.n <= a.n && (b.n == 0 || memcmp(a.p, b.p, (size_t)b.n) == 0);");
    行("}");
    行("static int 简_结尾(简字 a, 简字 b) {");
    行("    return b.n <= a.n && (b.n == 0 || memcmp(a.p + a.n - b.n, b.p, (size_t)b.n) == 0);");
    行("}");
    行("static int64_t 简_查找(简字 a, 简字 b) {");
    行("    if (b.n <= 0 || b.n > a.n) return -1;");
    行("    for (int64_t i = 0; i + b.n <= a.n; i++)");
    行("        if (memcmp(a.p + i, b.p, (size_t)b.n) == 0) return i;");
    行("    return -1;");
    行("}");
    行("static 简字 简_替换(简字 a, 简字 旧, 简字 新) {");
    行("    if (旧.n <= 0) return a;");
    行("    int64_t 数 = 0;");
    行("    for (int64_t i = 0; i + 旧.n <= a.n; ) {");
    行("        if (memcmp(a.p + i, 旧.p, (size_t)旧.n) == 0) { 数++; i += 旧.n; } else i++;");
    行("    }");
    行("    if (数 == 0) return a;");
    行("    int64_t 长 = a.n + 数 * (新.n - 旧.n);");
    行("    char *p = (char *)malloc((size_t)长 + 1);");
    行("    int64_t o = 0, i = 0;");
    行("    while (i < a.n) {");
    行("        if (i + 旧.n <= a.n && memcmp(a.p + i, 旧.p, (size_t)旧.n) == 0) {");
    行("            if (新.n) memcpy(p + o, 新.p, (size_t)新.n);");
    行("            o += 新.n; i += 旧.n;");
    行("        } else p[o++] = a.p[i++];");
    行("    }");
    行("    p[o] = 0;");
    行("    return 简_字(p, o);");
    行("}");
    行("static 简组 简_切分(简字 a, 简字 隔) {");
    行("    简组 g; g.元宽 = (int)sizeof(简字); g.n = 0; g.容 = 4;");
    行("    g.p = (char *)malloc((size_t)(g.容 * g.元宽));");
    行("    if (隔.n <= 0) { ((简字 *)g.p)[g.n++] = a; return g; }");
    行("    int64_t 起 = 0, i = 0;");
    行("    while (i + 隔.n <= a.n) {");
    行("        if (memcmp(a.p + i, 隔.p, (size_t)隔.n) == 0) {");
    行("            简_留够((简组 *)&g, g.n + 1);");
    行("            ((简字 *)g.p)[g.n++] = 简_字(a.p + 起, i - 起);");
    行("            i += 隔.n; 起 = i;");
    行("        } else i++;");
    行("    }");
    行("    简_留够((简组 *)&g, g.n + 1);");
    行("    ((简字 *)g.p)[g.n++] = 简_字(a.p + 起, a.n - 起);");
    行("    return g;");
    行("}");
    行("static 简字 简_字取(简字 s, int64_t i) {");
    行("    if (i < 0) return 简_字(\"\", 0);");
    行("    int64_t j = 0, k = 0;");
    行("    while (j < s.n && k < i) { j++; while (j < s.n && (s.p[j] & 0xC0) == 0x80) j++; k++; }");
    行("    if (j >= s.n) return 简_字(\"\", 0);");
    行("    int64_t 起 = j;");
    行("    j++; while (j < s.n && (s.p[j] & 0xC0) == 0x80) j++;");
    行("    return 简_字(s.p + 起, j - 起);");
    行("}");
    行("");
    行("/* ---- 图 ----");
    行("   像素是 RGBA8、行紧凑、左上原点。结构和字符串一样是轻量句柄：");
    行("   赋值不会复制像素，两个变量指向同一张图。需要独立副本就用 图.缩放。");
    行("   真正干活的函数在 lib简言图.a 里（stb_image / stb_image_write / nanosvg），");
    行("   这里只放声明——否则每编译一个 .jy 都要重编四十万行第三方代码。 */");
    行("typedef struct { int64_t 宽, 高; unsigned char *像素; } 简图;");
    行("extern unsigned char *简_图_载(const char *路径, long long *宽, long long *高, char *错);");
    行("extern int 简_图_存(const char *路径, const unsigned char *像素, long long 宽, long long 高);");
    行("extern unsigned char *简_图_新(long long 宽, long long 高);");
    行("extern void 简_图_放(unsigned char *像素);");
    行("extern unsigned char *简_图_缩(const unsigned char *源, long long 宽, long long 高,");
    行("                                long long 新宽, long long 新高);");
    行("extern long long 简_图_写(unsigned char *画布, long long 宽, long long 高,");
    行("                          long long x, long long y, double 字号,");
    行("                          const char *文字, long long 长, long long 颜色);");
    行("extern double 简_图_量(const char *文字, long long 长, double 字号);");
    行("static int64_t 简_图_取(简图 g, int64_t x, int64_t y) {");
    行("    if (!g.像素 || x < 0 || y < 0 || x >= g.宽 || y >= g.高) return 0;");
    行("    const unsigned char *p = g.像素 + ((size_t)y * (size_t)g.宽 + (size_t)x) * 4;");
    行("    return ((int64_t)p[0] << 24) | ((int64_t)p[1] << 16) | ((int64_t)p[2] << 8) | (int64_t)p[3];");
    行("}");
    行("static void 简_图_设(简图 g, int64_t x, int64_t y, int64_t c) {");
    行("    if (!g.像素 || x < 0 || y < 0 || x >= g.宽 || y >= g.高) return;");
    行("    unsigned char *p = g.像素 + ((size_t)y * (size_t)g.宽 + (size_t)x) * 4;");
    行("    p[0] = (unsigned char)((c >> 24) & 255);");
    行("    p[1] = (unsigned char)((c >> 16) & 255);");
    行("    p[2] = (unsigned char)((c >>  8) & 255);");
    行("    p[3] = (unsigned char)( c        & 255);");
    行("}");
    行("");
    行("/* 简言字符串是 (指针, 长度)，不保证 NUL 结尾——`切分`、`取` 出来的都是原串的切片。");
    行("   文件系统只认 C 字符串，所以每条路径都要先拷一份补上 0。");
    行("   不补的话，stb 会顺着缓冲区往下读，打开一个名字长得出乎意料的文件——");
    行("   或者干脆 ENOENT，而源文件里看着完全正常。 */");
    行("static char *简_路径(简字 s) {");
    行("    char *p = (char *)malloc((size_t)s.n + 1);");
    行("    if (!p) { fputs(\"内存不够了\\n\", stderr); exit(3); }");
    行("    if (s.n) memcpy(p, s.p, (size_t)s.n);");
    行("    p[s.n] = 0;");
    行("    return p;");
    行("}");
    行("");
    行("/* ---- 大整数与小数 ----");
    行("   两者在 C 里都只是一个指针，指向第三方库内部的结构（mp_int / mpd_t）。");
    行("   语言层永远看不见那个结构，所以用 void * 过桥：生成的 C 不 include");
    行("   任何第三方头文件，三万行 tommath 也就不必跟着每个 .jy 重编一遍。");
    行("");
    行("   三条约定，改之前先看规范第 13 节：");
    行("     1. 不可变 —— 运算都返回新对象，不改入参。所以复制指针是安全的，");
    行("        赋值、传参、当返回值都不用深拷贝。");
    行("     2. 不回收 —— 没有引用计数，对象活到进程结束（跟 图 一样）。");
    行("     3. 不崩 —— 整数除零得 0，负数开方得 0，指数越界得 0，小数除零得 无穷。");
    行("        错误的细节需要的话，自己用 比 和 位 去判。 */");
    行("typedef void *简大整;");
    行("typedef void *简小数;");
    行("extern void *简_大_从整(long long v);");
    行("extern void *简_大_读(const char *s, long long 长, char *错);");
    行("extern void *简_大_加(const void *a, const void *b);");
    行("extern void *简_大_减(const void *a, const void *b);");
    行("extern void *简_大_乘(const void *a, const void *b);");
    行("extern void *简_大_除(const void *a, const void *b);");
    行("extern void *简_大_余(const void *a, const void *b);");
    行("extern void *简_大_幂(const void *a, long long e);");
    行("extern void *简_大_根(const void *a);");
    行("extern long long 简_大_比(const void *a, const void *b);");
    行("extern long long 简_大_位(const void *a);");
    行("extern char *简_大_文(const void *a);");
    行("extern void 简_小_定精度(long long 位);");
    行("extern long long 简_小_取精度(void);");
    行("extern void *简_小_从浮(double v);");
    行("extern void *简_小_读(const char *s, long long 长, char *错);");
    行("extern void *简_小_加(const void *a, const void *b);");
    行("extern void *简_小_减(const void *a, const void *b);");
    行("extern void *简_小_乘(const void *a, const void *b);");
    行("extern void *简_小_除(const void *a, const void *b);");
    行("extern long long 简_小_比(const void *a, const void *b);");
    行("extern char *简_小_文(const void *a);");
    行("");
    行("/* ---- 声音 ----");
    行("   采样是 float32、交错存放；一帧 = 所有声道在同一时刻的采样。");
    行("   跟 图 一样是句柄：赋值只搬 32 字节的头，两个变量指向同一片采样。");
    行("   要独立副本就 .放大 / .归一化 —— 它们本来就要逐样本算，顺手拷一份。");
    行("   解码与编码在 lib简言图.a 里（dr_wav / dr_mp3），这里只有声明。 */");
    行("typedef struct { int64_t 帧数, 声道, 采样率; float *样本; } 简声;");
    行("extern float *简_声_载(const char *路径, long long *帧数, long long *声道,");
    行("                       long long *采样率, char *错);");
    行("extern int 简_声_存(const char *路径, const float *样本, long long 帧数,");
    行("                    long long 声道, long long 采样率);");
    行("extern float *简_声_新(long long 帧数, long long 声道);");
    行("extern float 简_声_取(const float *样本, long long 帧数, long long 声道数,");
    行("                      long long 帧, long long 声道);");
    行("extern void 简_声_设(float *样本, long long 帧数, long long 声道数,");
    行("                     long long 帧, long long 声道, double 值);");
    行("");
    行("static 简声 简_声_拷(简声 s) {");
    行("    简声 r = s;");
    行("    size_t 个 = (size_t)s.帧数 * (size_t)s.声道;");
    行("    if (!s.样本 || 个 == 0) { r.样本 = NULL; return r; }");
    行("    r.样本 = (float *)malloc(个 * sizeof(float));");
    行("    if (!r.样本) { fputs(\"声音：内存不够\\n\", stderr); exit(7); }");
    行("    memcpy(r.样本, s.样本, 个 * sizeof(float));");
    行("    return r;");
    行("}");
    行("/* 放大与归一化都先拷一份再改，原声音一个字节都不动——");
    行("   句柄语义下原地改会悄悄改掉调用方手里的那份。 */");
    行("static 简声 简_声_倍(简声 s, double k) {");
    行("    简声 r = 简_声_拷(s);");
    行("    size_t 个 = (size_t)r.帧数 * (size_t)r.声道, i;");
    行("    for (i = 0; i < 个; i++) r.样本[i] = (float)((double)r.样本[i] * k);");
    行("    return r;");
    行("}");
    行("static 简声 简_声_归(简声 s) {");
    行("    size_t 个 = (size_t)s.帧数 * (size_t)s.声道, i;");
    行("    double 峰 = 0.0;");
    行("    if (!s.样本 || 个 == 0) return s;");
    行("    for (i = 0; i < 个; i++) { double v = (double)s.样本[i]; if (v < 0) v = -v; if (v > 峰) 峰 = v; }");
    行("    if (峰 <= 0.0) return 简_声_拷(s);");
    行("    return 简_声_倍(s, 0.98 / 峰);");
    行("}");
    行("");
}

/* -------------------------------------------------------- 二元运算映射 -- */

/* 返回 C 运算符；不认识的返回 NULL */
static const char *C运算符(int 码)
{
    switch (码) {
    case 指_加: return "+";
    case 指_减: return "-";
    case 指_乘: return "*";
    case 指_整除: return "/";
    case 指_取余: return "%";
    case 指_位与: return "&";
    case 指_位或: return "|";
    case 指_位异或: return "^";
    case 指_左移: return "<<";
    case 指_右移: return ">>";
    case 指_等于: return "==";
    case 指_不等: return "!=";
    case 指_小于: return "<";
    case 指_大于: return ">";
    case 指_小于等: return "<=";
    case 指_大于等: return ">=";
    default: return NULL;
    }
}

static int 是浮点值(中间函数 *f, int 号)
{
    类型 *t = 值型(f, 号);
    return t && t->种 == 型_浮点;
}

/* ------------------------------------------------------------ 单条指令 -- */

static 中间函数 *找函数(const char *名, int 长);
static int 形参借用(中间函数 *f, int i);

static void 写指令(中间函数 *f, 指令 *q)
{
    int 果 = q->结果;

    switch (q->码) {

    /* ---- 常量 ---- */
    case 指_常量: {
        if (!q->型) { fprintf(stderr, "常量没类型\n"); exit(4); }
        if (q->型->种 == 型_字符串) {
            fprintf(出, "    ");
            值名(果);
            fprintf(出, " = 简_字(");
            /* 「空」在字符串上下文里是空串，不是两个字 */
            int 实长 = (q->文 && q->文长 == 3 && memcmp(q->文, "空", 3) == 0)
                     ? (fputs("\"\"", 出), 0)
                     : 写字面量(q->文 ? q->文 : "", q->文长);
            fprintf(出, ", %d);\n", 实长);
        } else if (q->型->种 == 型_空) {
            fprintf(出, "    ");
            值名(果);
            fprintf(出, " = 0;\n");
        } else if (q->型->种 == 型_布尔) {
            /* IR 里布尔的文可能是 真/假，也可能是 1/0 */
            int 真 = (q->文 && q->文长 == 3 && memcmp(q->文, "真", 3) == 0);
            fprintf(出, "    ");
            值名(果);
            fprintf(出, " = %d;\n", 真 ? 1 : (q->文 && q->文[0] == '1'));
        } else if (q->型->种 == 型_浮点) {
            fprintf(出, "    ");
            值名(果);
            fprintf(出, " = ");
            if (q->文 && q->文长) fwrite(q->文, 1, (size_t)q->文长, 出);
            else fputs("0.0", 出);
            fprintf(出, ";\n");
        } else if (q->型->种 == 型_命名 || q->型->种 == 型_组 ||
                   q->型->种 == 型_元组 || q->型->种 == 型_映射) {
            /* 结构/枚举/组的零值：C 里就是复合字面量的全零初始化。
               `私有 点 p` 没有初值时走这条。 */
            fprintf(出, "    ");
            值名(果);
            fprintf(出, " = (");
            fputs(C型(q->型), 出);
            fprintf(出, "){0};\n");
        } else {
            fprintf(出, "    ");
            值名(果);
            fprintf(出, " = ");
            /* 「空」在数值上下文里就是零值。遍历结束用的就是这个哨兵。 */
            if (q->文 && q->文长 == 3 && memcmp(q->文, "空", 3) == 0) fputs("0", 出);
            else if (q->文 && q->文长) fwrite(q->文, 1, (size_t)q->文长, 出);
            else fputs("0", 出);
            fprintf(出, "LL;\n");
        }
        return;
    }

    /* ---- 变量读写 ---- */
    case 指_载:
        /* 甲 是变量槽。载就是把它抄到一个临时值上。
         * 看着多余，但 IR 层要区分「读槽」和「用值」，这样寄存器和借用
         * 才好分析。到 C 里就是一次赋值，gcc 会直接消掉。 */
        fprintf(出, "    ");
        值名(果);
        fprintf(出, " = ");
        值名(q->甲);
        fprintf(出, ";\n");
        return;

    case 指_存:
        fprintf(出, "    ");
        值名(q->甲);
        fprintf(出, " = ");
        值名(q->乙);
        fprintf(出, ";\n");
        return;

    /* ---- 二元 ---- */
    case 指_加: case 指_减: case 指_乘:
    case 指_整除: case 指_取余:
    case 指_位与: case 指_位或: case 指_位异或:
    case 指_左移: case 指_右移:
    case 指_等于: case 指_不等:
    case 指_小于: case 指_大于: case 指_小于等: case 指_大于等: {
        const char *op = C运算符(q->码);
        if (!op) { fprintf(stderr, "不认识的二元运算\n"); exit(4); }
        /* 字符串的比较走 简_比较：C 的 strcmp 遇到 NUL 就停，
         * 简言字符串带长度，可以含 \0，所以不能直接用。 */
        int 是比较 = (q->码 == 指_等于 || q->码 == 指_不等 ||
                      q->码 == 指_小于 || q->码 == 指_大于 ||
                      q->码 == 指_小于等 || q->码 == 指_大于等);
        if (是比较 && 值型(f, q->甲) &&
            值型(f, q->甲)->种 == 型_字符串 &&
            值型(f, q->乙) && 值型(f, q->乙)->种 == 型_字符串) {
            fprintf(出, "    ");
            值名(果);
            fprintf(出, " = 简_比较(");
            值名(q->甲);
            fprintf(出, ", ");
            值名(q->乙);
            fprintf(出, ") %s 0;\n", op);
            return;
        }
        fprintf(出, "    ");
        值名(果);
        fprintf(出, " = ");
        值名(q->甲);
        fprintf(出, " %s ", op);
        值名(q->乙);
        fprintf(出, ";\n");
        return;
    }

    case 指_除:
        fprintf(出, "    ");
        值名(果);
        fprintf(出, " = (double)");
        值名(q->甲);
        fprintf(出, " / (double)");
        值名(q->乙);
        fprintf(出, ";\n");
        return;

    case 指_乘方:
        fprintf(出, "    ");
        值名(果);
        fprintf(出, " = 简_幂(");
        值名(q->甲);
        fprintf(出, ", ");
        值名(q->乙);
        fprintf(出, ");\n");
        return;

    case 指_拼接:
        fprintf(出, "    ");
        值名(果);
        fprintf(出, " = 简_拼(");
        值名(q->甲);
        fprintf(出, ", ");
        值名(q->乙);
        fprintf(出, ");\n");
        return;

    /* ---- 一元 ---- */
    case 指_非:
        fprintf(出, "    ");
        值名(果);
        fprintf(出, " = !");
        值名(q->甲);
        fprintf(出, ";\n");
        return;

    case 指_负:
        fprintf(出, "    ");
        值名(果);
        fprintf(出, " = -");
        值名(q->甲);
        fprintf(出, ";\n");
        return;

    case 指_取反:
        fprintf(出, "    ");
        值名(果);
        fprintf(出, " = ~");
        值名(q->甲);
        fprintf(出, ";\n");
        return;

    /* ---- 控制流 ---- */
    case 指_跳:
        fprintf(出, "    goto L%d;\n", q->甲);
        return;

    case 指_条件跳:
        fprintf(出, "    if (");
        值名(q->甲);
        fprintf(出, ") goto L%d; else goto L%d;\n", q->乙, q->丙);
        return;

    case 指_返回:
        if (q->甲 >= 0 && f->型 && f->型->返回 && f->型->返回->种 != 型_空) {
            fprintf(出, "    return ");
            值名(q->甲);
            fprintf(出, ";\n");
        } else if (f->型 && f->型->返回 && f->型->返回->种 != 型_空) {
            /* 函数末尾没写 返回（比如全是 匹配 分支）：给一个零值交差。
             * 语义层已经查过所有路径，走到这里说明是编译器补的终止块。 */
            fprintf(出, "    { %s _零 = {0}; return _零; }\n", C型(f->型->返回));
        } else {
            fprintf(出, "    return;\n");
        }
        return;

    case 指_空操作:
        return;

    case 指_丢弃:
        return;

    /* ---- 调用 ---- */
    case 指_调用: {
        /* 下面按名字展开的都是"编译器自带的"函数。
         * 但用户完全可以定义同名函数（`函数 开方(x: 浮点)` 是很自然的写法），
         * 而语义层已经允许用户顶掉内置符号。后端必须跟着让路——
         * 否则会出现最难查的一类 bug：源码里明明调的是自己的函数，
         * 生成的 C 里却是 libm 的 sqrt，两个都对、行为不同。 */
        int 用户函数 = (q->文 && 找函数(q->文, q->文长)) ? 1 : 0;

        int 内置 = 0;
        if (q->文 && q->文长 == 6 && memcmp(q->文, "打印", 6) == 0) 内置 = 1;
        else if (q->文 && q->文长 == 9 && memcmp(q->文, "打印行", 9) == 0) 内置 = 2;
        else if (q->文 && q->文长 == 6 && memcmp(q->文, "退出", 6) == 0) 内置 = 3;

        if (内置 == 1 || 内置 == 2) {
            for (int i = 0; i < q->多量; i++) {
                类型 *t = 值型(f, q->多[i]);
                if (t && t->种 == 型_字符串) {
                    fprintf(出, "    简_打印(");
                    值名(q->多[i]);
                    fprintf(出, ");\n");
                } else if (t && t->种 == 型_浮点) {
                    fprintf(出, "    printf(\"%%g\", (double)");
                    值名(q->多[i]);
                    fprintf(出, ");\n");
                } else if (t && t->种 == 型_布尔) {
                    fprintf(出, "    fputs(");
                    值名(q->多[i]);
                    fprintf(出, " ? \"真\" : \"假\", stdout);\n");
                } else {
                    fprintf(出, "    printf(\"%%lld\", (long long)");
                    值名(q->多[i]);
                    fprintf(出, ");\n");
                }
            }
            if (内置 == 2) fprintf(出, "    fputc('\\n', stdout);\n");
            if (果 >= 0) { fprintf(出, "    "); 值名(果); fprintf(出, " = 0;\n"); }
            return;
        }
        if (内置 == 3) {
            fprintf(出, "    exit((int)");
            if (q->多量 > 0) 值名(q->多[0]); else fputs("0", 出);
            fprintf(出, ");\n");
            return;
        }

        /* ---- 数学：直接发 libm 调用 ----
         * 这一层只是"名字翻译"，没有自己的实现。好处是拿到的就是 libm 的
         * 精度和速度，也包括它那些边界行为（sqrt(-1) = NaN、atan2(0,0) = 0）。 */
        if (q->文 && !用户函数) {
            static const struct { const char *名; const char *c; int 数; } 数学[] = {
                { "开方",     "sqrt",    1 },
                { "正弦",     "sin",     1 },
                { "余弦",     "cos",     1 },
                { "正切",     "tan",     1 },
                { "反正弦",   "asin",    1 },
                { "反余弦",   "acos",    1 },
                { "反正切",   "atan2",   2 },
                { "指数",     "exp",     1 },
                { "对数",     "log",     1 },
                { "对数10",   "log10",   1 },
                { "幂",       "pow",     2 },
                { "最小",     "fmin",    2 },
                { "最大",     "fmax",    2 },
                { "向下取整", "floor",   1 },
                { "向上取整", "ceil",    1 },
                { "绝对值",   "fabs",    1 },
            };
            for (int i = 0; i < (int)(sizeof(数学) / sizeof(数学[0])); i++) {
                if (!名同(q->文, q->文长, 数学[i].名, (int)strlen(数学[i].名))) continue;
                fprintf(出, "    ");
                if (果 >= 0) { 值名(果); fprintf(出, " = "); }
                fprintf(出, "%s((double)", 数学[i].c);
                for (int k = 0; k < 数学[i].数 && k < q->多量; k++) {
                    if (k) fputs(", (double)", 出);
                    值名(q->多[k]);
                }
                fprintf(出, ");\n");
                return;
            }
            if (名同(q->文, q->文长, "四舍五入", 12)) {
                fprintf(出, "    ");
                if (果 >= 0) { 值名(果); fprintf(出, " = "); }
                fprintf(出, "(long long)llround((double)");
                if (q->多量 > 0) 值名(q->多[0]); else fputs("0", 出);
                fprintf(出, ");\n");
                return;
            }
        }

        /* ---- 图片：三个自由函数 ----
         * 都是"一个语句块"：里面要开临时变量放宽高和错误信息。
         * 失败信息必须拷到堆上再塞进 结果 —— 简_果造 只按位拷贝那个 简字 结构体，
         * 指针还是指向栈上的 char 数组，函数一返回就失效了。 */
        if (q->文 && !用户函数 && 名同(q->文, q->文长, "读图", 6) && q->多量 >= 1) {
            fprintf(出, "    { long long _w = 0, _h = 0; char _e[256]; _e[0] = 0;\n");
            fprintf(出, "      char *_path = 简_路径(");
            值名(q->多[0]);
            fprintf(出, ");\n");
            fprintf(出, "      unsigned char *_p = 简_图_载(_path, &_w, &_h, _e);\n");
            fprintf(出, "      free(_path);\n");
            fprintf(出, "      if (_p) { 简图 _g; _g.宽 = _w; _g.高 = _h; _g.像素 = _p;\n");
            fprintf(出, "        ");
            值名(果);
            fprintf(出, " = 简_果造(1, &_g, (int)sizeof(_g)); }\n");
            fprintf(出, "      else { size_t _n = strlen(_e); char *_d = (char *)malloc(_n + 1);\n");
            fprintf(出, "        memcpy(_d, _e, _n + 1); 简字 _m = 简_字(_d, (int64_t)_n);\n");
            fprintf(出, "        ");
            值名(果);
            fprintf(出, " = 简_果造(0, &_m, (int)sizeof(_m)); } }\n");
            return;
        }
        if (q->文 && !用户函数 && 名同(q->文, q->文长, "存图", 6) && q->多量 >= 2) {
            fprintf(出, "    { char *_path = 简_路径(");
            值名(q->多[1]);
            fprintf(出, ");\n");
            fprintf(出, "      int _ok = 简_图_存(_path, ");
            值名(q->多[0]);
            fprintf(出, ".像素, ");
            值名(q->多[0]);
            fprintf(出, ".宽, ");
            值名(q->多[0]);
            fprintf(出, ".高);\n");
            fprintf(出, "      free(_path);\n");
            fprintf(出, "      if (_ok)  ");
            值名(果);
            fprintf(出, " = 简_果造(1, 0, 0);\n");
            fprintf(出, "      else { static const char _msg[] = ");
            fprintf(出, "\"存不进去：检查扩展名（png/jpg/bmp/tga）和目录权限\";\n");
            fprintf(出, "        简字 _m = 简_字(_msg, (int64_t)strlen(_msg));\n");
            fprintf(出, "        ");
            值名(果);
            fprintf(出, " = 简_果造(0, &_m, (int)sizeof(_m)); } }\n");
            return;
        }
        if (q->文 && !用户函数 && 名同(q->文, q->文长, "新建图", 9) && q->多量 >= 2) {
            fprintf(出, "    { 简图 _g; _g.宽 = (int64_t)");
            值名(q->多[0]);
            fprintf(出, "; _g.高 = (int64_t)");
            值名(q->多[1]);
            fprintf(出, "; _g.像素 = 简_图_新(_g.宽, _g.高);\n");
            fprintf(出, "      if (!_g.像素) { fputs(\"新建图：宽高要大于 0，内存也可能不够\\n\", stderr); exit(7); }\n");
            fprintf(出, "      ");
            值名(果);
            fprintf(出, " = _g; }\n");
            return;
        }

        /* ---- 声音：两个自由函数 ----
         * 音频的"路径"是文件路径，同样要补 NUL 才能交给 fopen。 */
        if (q->文 && !用户函数 && 名同(q->文, q->文长, "载入声音", 12) && q->多量 >= 1) {
            fprintf(出, "    { long long _n = 0, _ch = 0, _sr = 0; char _e[256]; _e[0] = 0;\n");
            fprintf(出, "      char *_path = 简_路径(");
            值名(q->多[0]);
            fprintf(出, ");\n");
            fprintf(出, "      float *_p = 简_声_载(_path, &_n, &_ch, &_sr, _e);\n");
            fprintf(出, "      free(_path);\n");
            fprintf(出, "      if (_p) { 简声 _s; _s.帧数 = _n; _s.声道 = _ch; _s.采样率 = _sr; _s.样本 = _p;\n");
            fprintf(出, "        ");
            值名(果);
            fprintf(出, " = 简_果造(1, &_s, (int)sizeof(_s)); }\n");
            fprintf(出, "      else { size_t _k = strlen(_e); char *_d = (char *)malloc(_k + 1);\n");
            fprintf(出, "        memcpy(_d, _e, _k + 1); 简字 _m = 简_字(_d, (int64_t)_k);\n");
            fprintf(出, "        ");
            值名(果);
            fprintf(出, " = 简_果造(0, &_m, (int)sizeof(_m)); } }\n");
            return;
        }
        if (q->文 && !用户函数 && 名同(q->文, q->文长, "新建声音", 12) && q->多量 >= 3) {
            fprintf(出, "    { 简声 _s; _s.帧数 = (int64_t)");
            值名(q->多[0]);
            fprintf(出, "; _s.采样率 = (int64_t)");
            值名(q->多[1]);
            fprintf(出, "; _s.声道 = (int64_t)");
            值名(q->多[2]);
            fprintf(出, ";\n");
            fprintf(出, "      _s.样本 = 简_声_新(_s.帧数, _s.声道);\n");
            fprintf(出, "      if (!_s.样本) { fputs(\"新建声音：帧数、声道、采样率都要大于 0\\n\", stderr); exit(7); }\n");
            fprintf(出, "      ");
            值名(果);
            fprintf(出, " = _s; }\n");
            return;
        }

        /* ---- 大数与小数 ----
         *
         * 六个入口全是"薄包装"：真正算的地方在 lib简言图.a 里。
         * 读类函数（读大数/读小数）可能失败，因此和 读图 一样包一层 结果，
         * 失败信息同样要拷到堆上——简_果造 只按位拷贝 简字 结构体。 */
        if (q->文 && !用户函数 && 名同(q->文, q->文长, "大数", 6) && q->多量 >= 1) {
            fprintf(出, "    ");
            if (果 >= 0) { 值名(果); fprintf(出, " = "); }
            fprintf(出, "简_大_从整((long long)");
            值名(q->多[0]);
            fprintf(出, ");\n");
            return;
        }
        if (q->文 && !用户函数 && 名同(q->文, q->文长, "变小数", 9) && q->多量 >= 1) {
            fprintf(出, "    ");
            if (果 >= 0) { 值名(果); fprintf(出, " = "); }
            fprintf(出, "简_小_从浮((double)");
            值名(q->多[0]);
            fprintf(出, ");\n");
            return;
        }
        if (q->文 && !用户函数 && 名同(q->文, q->文长, "定小数精度", 15) && q->多量 >= 1) {
            fprintf(出, "    简_小_定精度((long long)");
            值名(q->多[0]);
            fprintf(出, ");\n");
            if (果 >= 0) { fprintf(出, "    "); 值名(果); fprintf(出, " = 0;\n"); }
            return;
        }
        if (q->文 && !用户函数 && 名同(q->文, q->文长, "小数精度", 12) && q->多量 == 0) {
            fprintf(出, "    ");
            if (果 >= 0) { 值名(果); fprintf(出, " = "); }
            fprintf(出, "简_小_取精度();\n");
            return;
        }
        if (q->文 && !用户函数 && 名同(q->文, q->文长, "读大数", 9) && q->多量 >= 1) {
            fprintf(出, "    { char _e[256]; _e[0] = 0;\n");
            fprintf(出, "      char *_s = 简_路径(");
            值名(q->多[0]);
            fprintf(出, ");\n");
            fprintf(出, "      void *_v = 简_大_读(_s, (long long)");
            值名(q->多[0]);
            fprintf(出, ".n, _e);\n");
            fprintf(出, "      free(_s);\n");
            fprintf(出, "      if (_v) { 简大整 _g = _v;\n        ");
            值名(果);
            fprintf(出, " = 简_果造(1, &_g, (int)sizeof(_g)); }\n");
            fprintf(出, "      else { size_t _n = strlen(_e); char *_d = (char *)malloc(_n + 1);\n");
            fprintf(出, "        memcpy(_d, _e, _n + 1); 简字 _m = 简_字(_d, (int64_t)_n);\n        ");
            值名(果);
            fprintf(出, " = 简_果造(0, &_m, (int)sizeof(_m)); } }\n");
            return;
        }
        if (q->文 && !用户函数 && 名同(q->文, q->文长, "读小数", 9) && q->多量 >= 1) {
            fprintf(出, "    { char _e[256]; _e[0] = 0;\n");
            fprintf(出, "      char *_s = 简_路径(");
            值名(q->多[0]);
            fprintf(出, ");\n");
            fprintf(出, "      void *_v = 简_小_读(_s, (long long)");
            值名(q->多[0]);
            fprintf(出, ".n, _e);\n");
            fprintf(出, "      free(_s);\n");
            fprintf(出, "      if (_v) { 简小数 _g = _v;\n        ");
            值名(果);
            fprintf(出, " = 简_果造(1, &_g, (int)sizeof(_g)); }\n");
            fprintf(出, "      else { size_t _n = strlen(_e); char *_d = (char *)malloc(_n + 1);\n");
            fprintf(出, "        memcpy(_d, _e, _n + 1); 简字 _m = 简_字(_d, (int64_t)_n);\n        ");
            值名(果);
            fprintf(出, " = 简_果造(0, &_m, (int)sizeof(_m)); } }\n");
            return;
        }

        /* 文 为空：被调是一个函数值（函数指针） */
        if (!q->文) {
            int 无值 = (q->型 && q->型->种 == 型_空);
            fprintf(出, "    ");
            if (!无值 && 果 >= 0) {
                值名(果);
                fprintf(出, " = ");
            }
            值名(q->甲);
            fputc('(', 出);
            if (q->多量 == 0) fputs("void", 出);
            for (int i = 0; i < q->多量; i++) {
                if (i) fputs(", ", 出);
                值名(q->多[i]);
            }
            fprintf(出, ");\n");
            return;
        }

        /* 普通调用：文 是函数名。
         * 返回空值的函数不能写成 `v = f()`——C 里 void 值不能赋给变量。 */
        int 无值 = (q->型 && q->型->种 == 型_空);
        fprintf(出, "    ");
        if (!无值 && 果 >= 0) {
            值名(果);
            fprintf(出, " = ");
        }
        写函数名(q->文, q->文长);
        fputc('(', 出);
        中间函数 *被调 = 找函数(q->文, q->文长);
        for (int i = 0; i < q->多量; i++) {
            if (i) fputs(", ", 出);
            if (被调 && 形参借用(被调, i)) 取址名(q->多[i]);
            else 值名(q->多[i]);
        }
        fprintf(出, ");\n");
        return;
    }

    /* ---- 内置属性（零参方法） ---- *
     * `词.长度`、`结果.是成功` 这类没有括号的读法。语义层已经认定它们是
     * 属性不是函数，这里按对象类型展开。 */
    case 指_取属性: {
        类型 *ot = 值型(f, q->甲);
        const char *m = q->文 ? q->文 : "";
        int ml = q->文长;

        if (ot && ot->种 == 型_字符串 &&
            (名同(m, ml, "长度", 6) || 名同(m, ml, "字节长度", 12))) {
            fprintf(出, "    ");
            值名(果);
            fprintf(出, " = %s(",
                    名同(m, ml, "长度", 6) ? "简_字数" : "简_字节数");
            值名(q->甲);
            fprintf(出, ");\n");
            return;
        }
        if (ot && (ot->种 == 型_组 || ot->种 == 型_映射)) {
            fprintf(出, "    ");
            值名(果);
            fprintf(出, " = ");
            值名(q->甲);
            fprintf(出, ".n;\n");
            return;
        }
        if (ot && ot->种 == 型_结果) {
            if (名同(m, ml, "是成功", 9)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = ");
                值名(q->甲);
                fprintf(出, ".成;\n");
                return;
            }
            if (名同(m, ml, "是失败", 9)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = !");
                值名(q->甲);
                fprintf(出, ".成;\n");
                return;
            }
            if (名同(m, ml, "取值", 6) || 名同(m, ml, "错误值", 9)) {
                const char *ct = C型(q->型);
                /* 载荷固定 128 字节，而目标类型可能比它大（大结构体当结果值）。
                 * 按 min(载荷宽, 目标宽) 拷，剩下的保持 memset 的零。 */
                fprintf(出, "    { %s _r; memset(&_r, 0, sizeof(_r)); size_t _k = (size_t)(", ct);
                值名(q->甲);
                fprintf(出, ".宽 < (int)sizeof(_r) ? ");
                值名(q->甲);
                fprintf(出, ".宽 : (int)sizeof(_r)); memcpy(&_r, ");
                值名(q->甲);
                fprintf(出, ".p, _k); ");
                值名(果);
                fprintf(出, " = _r; }\n");
                return;
            }
        }

        if (ot && ot->种 == 型_图 && (名同(m, ml, "宽", 3) || 名同(m, ml, "高", 3))) {
            fprintf(出, "    ");
            值名(果);
            fprintf(出, " = ");
            值名(q->甲);
            fprintf(出, ".%s;\n", 名同(m, ml, "宽", 3) ? "宽" : "高");
            return;
        }

        if (ot && ot->种 == 型_声音) {
            if (名同(m, ml, "帧数", 6) || 名同(m, ml, "采样率", 9) || 名同(m, ml, "声道", 6)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = ");
                值名(q->甲);
                fprintf(出, ".%s;\n",
                        名同(m, ml, "帧数", 6) ? "帧数" :
                        名同(m, ml, "采样率", 9) ? "采样率" : "声道");
                return;
            }
            if (名同(m, ml, "时长", 6)) {
                /* 采样率为 0 只在构造失败时出现，那时 帧数 也是 0，给 0.0 就行 */
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = ");
                值名(q->甲);
                fprintf(出, ".采样率 > 0 ? (double)");
                值名(q->甲);
                fprintf(出, ".帧数 / (double)");
                值名(q->甲);
                fprintf(出, ".采样率 : 0.0;\n");
                return;
            }
        }

        if (ot && ot->种 == 型_可空) {
            if (名同(m, ml, "有值", 6)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = (");
                值名(q->甲);
                fprintf(出, ".n != 0);\n");
                return;
            }
        }
        fprintf(stderr, "后端还没实现属性：%.*s\n", ml, m);
        exit(4);
    }

    /* ---- 内置方法调用 ---- *
     * 组和字符串的方法是语言自带的能力，不走用户代码，直接展开成运行时调用。
     * 展开而不是包一层函数，是因为元素类型只有这里知道：
     * 同一个「取」，对 整数组 是 long long，对 字符串组 是 简字。 */
    case 指_调用方法: {
        类型 *ot = 值型(f, q->甲);
        const char *m = q->文 ? q->文 : "";
        int ml = q->文长;
        int 是组 = ot && ot->种 == 型_组;
        int 是串 = ot && ot->种 == 型_字符串;

        /* 数字转文字。语义层的内置表早就登记了，后端一直没接——
         * 症状是写 `n.转字符串()` 直接报「后端还没实现方法」。
         * 运行时里 简_整字 / 简_浮字 一开始就有（打印内部在用），接上即可。 */
        if (名同(m, ml, "转字符串", 12)) {
            if (ot && ot->种 == 型_整数) {
                fprintf(出, "    ");
                if (果 >= 0) 值名(果);
                fprintf(出, " = 简_整字((long long)");
                值名(q->甲);
                fprintf(出, ");\n");
                return;
            }
            if (ot && ot->种 == 型_浮点) {
                fprintf(出, "    ");
                if (果 >= 0) 值名(果);
                fprintf(出, " = 简_浮字((double)");
                值名(q->甲);
                fprintf(出, ");\n");
                return;
            }
        }

        if (是组) {
            类型 *元 = ot->元素;
            const char *et = C型(元);
            int 类 = (元 && 元->种 == 型_浮点) ? 2 : (元 && 元->种 == 型_字符串) ? 3 : 1;

            if (名同(m, ml, "追加", 6)) {
                fprintf(出, "    简_追加(&");
                值名(q->甲);
                fprintf(出, ", &");
                值名(q->多[0]);
                fprintf(出, ");\n");
                if (果 >= 0) { fprintf(出, "    "); 值名(果); fprintf(出, " = 0;\n"); }
                return;
            }
            if (名同(m, ml, "插入", 6)) {
                fprintf(出, "    简_插入(&");
                值名(q->甲);
                fprintf(出, ", ");
                值名(q->多[0]);
                fprintf(出, ", &");
                值名(q->多[1]);
                fprintf(出, ");\n");
                if (果 >= 0) { fprintf(出, "    "); 值名(果); fprintf(出, " = 0;\n"); }
                return;
            }
            if (名同(m, ml, "包含", 6)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = 简_含(&");
                值名(q->甲);
                fprintf(出, ", &");
                值名(q->多[0]);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "取", 3)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = (");
                值名(q->多[0]);
                fprintf(出, " >= 0 && ");
                值名(q->多[0]);
                fprintf(出, " < ");
                值名(q->甲);
                fprintf(出, ".n) ? ((");
                fputs(et, 出);
                fprintf(出, " *)");
                值名(q->甲);
                fprintf(出, ".p)[");
                值名(q->多[0]);
                fprintf(出, "] : (");
                fputs(et, 出);
                fprintf(出, "){0};\n");
                return;
            }
            if (名同(m, ml, "删除", 6) || 名同(m, ml, "弹出", 6)) {
                int 弹 = 名同(m, ml, "弹出", 6);
                fprintf(出, "    { int64_t _i = ");
                if (弹) {
                    值名(q->甲);
                    fprintf(出, ".n - 1");
                } else {
                    值名(q->多[0]);
                }
                fprintf(出, "; ");
                值名(果);
                fprintf(出, " = (");
                fputs(et, 出);
                fprintf(出, "){0};");
                fprintf(出, " if (_i >= 0 && _i < ");
                值名(q->甲);
                fprintf(出, ".n) { ");
                值名(果);
                fprintf(出, " = ((");
                fputs(et, 出);
                fprintf(出, " *)");
                值名(q->甲);
                fprintf(出, ".p)[_i]; memmove(");
                值名(q->甲);
                fprintf(出, ".p + _i * ");
                值名(q->甲);
                fprintf(出, ".元宽, ");
                值名(q->甲);
                fprintf(出, ".p + (_i + 1) * ");
                值名(q->甲);
                fprintf(出, ".元宽, (size_t)((");
                值名(q->甲);
                fprintf(出, ".n - _i - 1) * ");
                值名(q->甲);
                fprintf(出, ".元宽)); ");
                值名(q->甲);
                fprintf(出, ".n--; } }\n");
                return;
            }
            if (名同(m, ml, "清空", 6)) {
                fprintf(出, "    简_清空(&");
                值名(q->甲);
                fprintf(出, ");\n");
                if (果 >= 0) { fprintf(出, "    "); 值名(果); fprintf(出, " = 0;\n"); }
                return;
            }
            if (名同(m, ml, "复制", 6)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = 简_复制组(");
                值名(q->甲);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "反转", 6)) {
                fprintf(出, "    { int64_t _i = 0, _j = ");
                值名(q->甲);
                fprintf(出, ".n - 1; while (_i < _j) { ");
                fputs("char *_a = ", 出);
                值名(q->甲);
                fprintf(出, ".p + _i * ");
                值名(q->甲);
                fprintf(出, ".元宽, *_b = ");
                值名(q->甲);
                fprintf(出, ".p + _j * ");
                值名(q->甲);
                fprintf(出, ".元宽; for (int _k = 0; _k < ");
                值名(q->甲);
                fprintf(出, ".元宽; _k++) { char _t = _a[_k]; _a[_k] = _b[_k]; _b[_k] = _t; } _i++; _j--; } }\n");
                if (果 >= 0) { fprintf(出, "    "); 值名(果); fprintf(出, " = 0;\n"); }
                return;
            }
            if (名同(m, ml, "排序", 6)) {
                fprintf(出, "    简_排序(&");
                值名(q->甲);
                fprintf(出, ", %d);\n", 类);
                if (果 >= 0) { fprintf(出, "    "); 值名(果); fprintf(出, " = 0;\n"); }
                return;
            }
            if (名同(m, ml, "长度", 6)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = ");
                值名(q->甲);
                fprintf(出, ".n;\n");
                return;
            }
        }

        if (ot && ot->种 == 型_结果) {
            if (名同(m, ml, "是成功", 9)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = ");
                值名(q->甲);
                fprintf(出, ".成;\n");
                return;
            }
            if (名同(m, ml, "是失败", 9)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = !");
                值名(q->甲);
                fprintf(出, ".成;\n");
                return;
            }
            if (名同(m, ml, "取值", 6) || 名同(m, ml, "错误值", 9)) {
                const char *ct = C型(q->型);
                /* 载荷固定 128 字节，而目标类型可能比它大（大结构体当结果值）。
                 * 按 min(载荷宽, 目标宽) 拷，剩下的保持 memset 的零。 */
                fprintf(出, "    { %s _r; memset(&_r, 0, sizeof(_r)); size_t _k = (size_t)(", ct);
                值名(q->甲);
                fprintf(出, ".宽 < (int)sizeof(_r) ? ");
                值名(q->甲);
                fprintf(出, ".宽 : (int)sizeof(_r)); memcpy(&_r, ");
                值名(q->甲);
                fprintf(出, ".p, _k); ");
                值名(果);
                fprintf(出, " = _r; }\n");
                return;
            }
        }

        if (ot && ot->种 == 型_可空) {
            类型 *元 = ot->元素;
            int 串空 = 元 && 元->种 == 型_字符串;
            if (名同(m, ml, "或默认", 9)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = (");
                值名(q->甲);
                fprintf(出, 串空 ? ".n ? " : " ? ");
                值名(q->甲);
                fprintf(出, " : ");
                值名(q->多[0]);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "有值", 6)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = (");
                值名(q->甲);
                fprintf(出, 串空 ? ".n != 0);\n" : " != 0);\n");
                return;
            }
            if (名同(m, ml, "取值", 6)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = ");
                值名(q->甲);
                fprintf(出, ";\n");
                return;
            }
        }

        if (ot && ot->种 == 型_映射) {
            类型 *vt = ot->值;
            const char *vtx = C型(vt);
            if (名同(m, ml, "取", 3)) {
                fprintf(出, "    { %s _r; 简_映取(&", vtx);
                值名(q->甲);
                fprintf(出, ", &");
                值名(q->多[0]);
                fprintf(出, ", &_r); ");
                值名(果);
                fprintf(出, " = _r; }\n");
                return;
            }
            if (名同(m, ml, "插入", 6)) {
                fprintf(出, "    { %s _o; 简_映插(&", vtx);
                值名(q->甲);
                fprintf(出, ", &");
                值名(q->多[0]);
                fprintf(出, ", &");
                值名(q->多[1]);
                fprintf(出, ", &_o); ");
                值名(果);
                fprintf(出, " = _o; }\n");
                return;
            }
            if (名同(m, ml, "包含", 6)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = 简_映含(&");
                值名(q->甲);
                fprintf(出, ", &");
                值名(q->多[0]);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "删除", 6)) {
                fprintf(出, "    { %s _o; 简_映删(&", vtx);
                值名(q->甲);
                fprintf(出, ", &");
                值名(q->多[0]);
                fprintf(出, ", &_o); ");
                值名(果);
                fprintf(出, " = _o; }\n");
                return;
            }
            if (名同(m, ml, "键组", 6)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = 简_映键(&");
                值名(q->甲);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "值组", 6)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = 简_映值(&");
                值名(q->甲);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "清空", 6)) {
                fprintf(出, "    ");
                值名(q->甲);
                fprintf(出, ".n = 0;\n");
                if (果 >= 0) { fprintf(出, "    "); 值名(果); fprintf(出, " = 0;\n"); }
                return;
            }
        }

        if (ot && ot->种 == 型_图) {
            if (名同(m, ml, "取", 3)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = 简_图_取(");
                值名(q->甲);
                fprintf(出, ", ");
                值名(q->多[0]);
                fprintf(出, ", ");
                值名(q->多[1]);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "设", 3)) {
                fprintf(出, "    简_图_设(");
                值名(q->甲);
                fprintf(出, ", ");
                值名(q->多[0]);
                fprintf(出, ", ");
                值名(q->多[1]);
                fprintf(出, ", ");
                值名(q->多[2]);
                fprintf(出, ");\n");
                if (果 >= 0) { fprintf(出, "    "); 值名(果); fprintf(出, " = 0;\n"); }
                return;
            }
            if (名同(m, ml, "存", 3)) {
                fprintf(出, "    { char *_path = 简_路径(");
                值名(q->多[0]);
                fprintf(出, ");\n");
                fprintf(出, "      int _ok = 简_图_存(_path, ");
                值名(q->甲);
                fprintf(出, ".像素, ");
                值名(q->甲);
                fprintf(出, ".宽, ");
                值名(q->甲);
                fprintf(出, ".高);\n");
                fprintf(出, "      free(_path);\n");
                fprintf(出, "      if (_ok)  ");
                值名(果);
                fprintf(出, " = 简_果造(1, 0, 0);\n");
                fprintf(出, "      else { static const char _msg[] = ");
                fprintf(出, "\"存不进去：检查扩展名（png/jpg/bmp/tga）和目录权限\";\n");
                fprintf(出, "        简字 _m = 简_字(_msg, (int64_t)strlen(_msg));\n");
                fprintf(出, "        ");
                值名(果);
                fprintf(出, " = 简_果造(0, &_m, (int)sizeof(_m)); } }\n");
                return;
            }
            if (名同(m, ml, "写字", 6)) {
                /* 四个参数里前三个是数，第四个是 (指针, 长度) 的字符串。
                 * 直接把 .p 和 .n 拆开传给 C——不要转成临时 NUL 串，
                 * stb 这边收长度，本来就不需要结尾 0。 */
                fprintf(出, "    ");
                if (果 >= 0) { 值名(果); fprintf(出, " = "); }
                fprintf(出, "(long long)简_图_写(");
                值名(q->甲);
                fprintf(出, ".像素, ");
                值名(q->甲);
                fprintf(出, ".宽, ");
                值名(q->甲);
                fprintf(出, ".高, (long long)");
                值名(q->多[0]);
                fprintf(出, ", (long long)");
                值名(q->多[1]);
                fprintf(出, ", (double)");
                值名(q->多[2]);
                fprintf(出, ", ");
                值名(q->多[3]);
                fprintf(出, ".p, ");
                值名(q->多[3]);
                fprintf(出, ".n, (long long)");
                值名(q->多[4]);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "量字", 6)) {
                fprintf(出, "    ");
                if (果 >= 0) { 值名(果); fprintf(出, " = "); }
                fprintf(出, "简_图_量(");
                值名(q->多[0]);
                fprintf(出, ".p, ");
                值名(q->多[0]);
                fprintf(出, ".n, (double)");
                值名(q->多[1]);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "缩放", 6)) {
                /* 缩放是唯一会真的复制像素的操作。
                 * 赋值、传参都只搬那个 24 字节的句柄，这是"图是共享的"最省事的边界。 */
                fprintf(出, "    { 简图 _g = ");
                值名(q->甲);
                fprintf(出, ";\n");
                fprintf(出, "      unsigned char *_p = 简_图_缩(_g.像素, _g.宽, _g.高, ");
                值名(q->多[0]);
                fprintf(出, ", ");
                值名(q->多[1]);
                fprintf(出, ");\n");
                fprintf(出, "      if (!_p) { fputs(\"缩放：新宽高要大于 0，内存也可能不够\\n\", stderr); exit(7); }\n");
                fprintf(出, "      ");
                值名(果);
                fprintf(出, ".宽 = ");
                值名(q->多[0]);
                fprintf(出, "; ");
                值名(果);
                fprintf(出, ".高 = ");
                值名(q->多[1]);
                fprintf(出, "; ");
                值名(果);
                fprintf(出, ".像素 = _p; }\n");
                return;
            }
            fprintf(stderr, "后端还没实现图的方法：%.*s\n", ml, m);
            exit(4);
        }

        if (ot && ot->种 == 型_声音) {
            if (名同(m, ml, "取", 3)) {
                fprintf(出, "    ");
                if (果 >= 0) { 值名(果); fprintf(出, " = (double)"); }
                fprintf(出, "简_声_取(");
                值名(q->甲);
                fprintf(出, ".样本, ");
                值名(q->甲);
                fprintf(出, ".帧数, ");
                值名(q->甲);
                fprintf(出, ".声道, ");
                值名(q->多[0]);
                fprintf(出, ", ");
                值名(q->多[1]);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "设", 3)) {
                fprintf(出, "    简_声_设(");
                值名(q->甲);
                fprintf(出, ".样本, ");
                值名(q->甲);
                fprintf(出, ".帧数, ");
                值名(q->甲);
                fprintf(出, ".声道, ");
                值名(q->多[0]);
                fprintf(出, ", ");
                值名(q->多[1]);
                fprintf(出, ", (double)");
                值名(q->多[2]);
                fprintf(出, ");\n");
                if (果 >= 0) { fprintf(出, "    "); 值名(果); fprintf(出, " = 0;\n"); }
                return;
            }
            if (名同(m, ml, "混", 3)) {
                /* 累加而不是覆盖：合成和弦时，三个正弦要落在同一帧上。
                 * 边界检查照抄 简_声_设 的口径，越界就当没发生。 */
                fprintf(出, "    { 简声 _s = ");
                值名(q->甲);
                fprintf(出, "; long long _f = (long long)");
                值名(q->多[0]);
                fprintf(出, ", _c = (long long)");
                值名(q->多[1]);
                fprintf(出, ";\n");
                fprintf(出, "      if (_s.样本 && _f >= 0 && _f < _s.帧数 && _c >= 0 && _c < _s.声道)\n");
                fprintf(出, "          _s.样本[_f * _s.声道 + _c] += (float)((double)");
                值名(q->多[2]);
                fprintf(出, "); }\n");
                if (果 >= 0) { fprintf(出, "    "); 值名(果); fprintf(出, " = 0;\n"); }
                return;
            }
            if (名同(m, ml, "存", 3)) {
                fprintf(出, "    { char *_path = 简_路径(");
                值名(q->多[0]);
                fprintf(出, ");\n");
                fprintf(出, "      int _ok = 简_声_存(_path, ");
                值名(q->甲);
                fprintf(出, ".样本, ");
                值名(q->甲);
                fprintf(出, ".帧数, ");
                值名(q->甲);
                fprintf(出, ".声道, ");
                值名(q->甲);
                fprintf(出, ".采样率);\n");
                fprintf(出, "      free(_path);\n");
                fprintf(出, "      if (_ok)  ");
                值名(果);
                fprintf(出, " = 简_果造(1, 0, 0);\n");
                fprintf(出, "      else { static const char _msg[] = ");
                fprintf(出, "\"存不进去：检查扩展名（wav）和目录权限\";\n");
                fprintf(出, "        简字 _m = 简_字(_msg, (int64_t)strlen(_msg));\n");
                fprintf(出, "        ");
                值名(果);
                fprintf(出, " = 简_果造(0, &_m, (int)sizeof(_m)); } }\n");
                return;
            }
            if (名同(m, ml, "放大", 6)) {
                fprintf(出, "    ");
                if (果 >= 0) { 值名(果); fprintf(出, " = "); }
                fprintf(出, "简_声_倍(");
                值名(q->甲);
                fprintf(出, ", (double)");
                值名(q->多[0]);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "归一化", 9)) {
                fprintf(出, "    ");
                if (果 >= 0) { 值名(果); fprintf(出, " = "); }
                fprintf(出, "简_声_归(");
                值名(q->甲);
                fprintf(出, ");\n");
                return;
            }
            fprintf(stderr, "后端还没实现声音的方法：%.*s\n", ml, m);
            exit(4);
        }

        if (ot && ot->种 == 型_大整数) {
            /* 二元四则 + 余：全是同一个模子，名字表和 C 函数名对得上 */
            static const struct { const char *名; const char *c; } 二元[] = {
                { "加", "简_大_加" }, { "减", "简_大_减" },
                { "乘", "简_大_乘" }, { "除", "简_大_除" },
                { "余", "简_大_余" },
            };
            for (int i = 0; i < (int)(sizeof(二元) / sizeof(二元[0])); i++) {
                if (!名同(m, ml, 二元[i].名, 3)) continue;
                fprintf(出, "    ");
                if (果 >= 0) { 值名(果); fprintf(出, " = "); }
                fprintf(出, "%s(", 二元[i].c);
                值名(q->甲);
                fprintf(出, ", ");
                值名(q->多[0]);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "幂", 3)) {
                fprintf(出, "    ");
                if (果 >= 0) { 值名(果); fprintf(出, " = "); }
                fprintf(出, "简_大_幂(");
                值名(q->甲);
                fprintf(出, ", (long long)");
                值名(q->多[0]);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "根", 3)) {
                fprintf(出, "    ");
                if (果 >= 0) { 值名(果); fprintf(出, " = "); }
                fprintf(出, "简_大_根(");
                值名(q->甲);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "比", 3)) {
                fprintf(出, "    ");
                if (果 >= 0) { 值名(果); fprintf(出, " = "); }
                fprintf(出, "简_大_比(");
                值名(q->甲);
                fprintf(出, ", ");
                值名(q->多[0]);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "位", 3)) {
                fprintf(出, "    ");
                if (果 >= 0) { 值名(果); fprintf(出, " = "); }
                fprintf(出, "简_大_位(");
                值名(q->甲);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "文本", 6)) {
                /* 简_大_文 给的是 malloc 出来的 NUL 串，长度现算。
                 * 不 free：简字 会跟着这个指针走，语言层没有 释放。 */
                fprintf(出, "    { char *_s = 简_大_文(");
                值名(q->甲);
                fprintf(出, ");\n      ");
                if (果 >= 0) {
                    值名(果);
                    fprintf(出, " = 简_字(_s, (int64_t)strlen(_s)); }\n");
                } else {
                    fprintf(出, "free(_s); }\n");
                }
                return;
            }
            fprintf(stderr, "后端还没实现大整数的方法：%.*s\n", ml, m);
            exit(4);
        }

        if (ot && ot->种 == 型_小数) {
            static const struct { const char *名; const char *c; } 二元[] = {
                { "加", "简_小_加" }, { "减", "简_小_减" },
                { "乘", "简_小_乘" }, { "除", "简_小_除" },
            };
            for (int i = 0; i < (int)(sizeof(二元) / sizeof(二元[0])); i++) {
                if (!名同(m, ml, 二元[i].名, 3)) continue;
                fprintf(出, "    ");
                if (果 >= 0) { 值名(果); fprintf(出, " = "); }
                fprintf(出, "%s(", 二元[i].c);
                值名(q->甲);
                fprintf(出, ", ");
                值名(q->多[0]);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "比", 3)) {
                fprintf(出, "    ");
                if (果 >= 0) { 值名(果); fprintf(出, " = "); }
                fprintf(出, "简_小_比(");
                值名(q->甲);
                fprintf(出, ", ");
                值名(q->多[0]);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "文本", 6)) {
                fprintf(出, "    { char *_s = 简_小_文(");
                值名(q->甲);
                fprintf(出, ");\n      ");
                if (果 >= 0) {
                    值名(果);
                    fprintf(出, " = 简_字(_s, (int64_t)strlen(_s)); }\n");
                } else {
                    fprintf(出, "free(_s); }\n");
                }
                return;
            }
            fprintf(stderr, "后端还没实现小数的方法：%.*s\n", ml, m);
            exit(4);
        }

        if (是串) {
            if (名同(m, ml, "长度", 6)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = 简_字数(");
                值名(q->甲);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "字节长度", 12)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = ");
                值名(q->甲);
                fprintf(出, ".n;\n");
                return;
            }
            if (名同(m, ml, "包含", 6)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = 简_字含(");
                值名(q->甲);
                fprintf(出, ", ");
                值名(q->多[0]);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "大写", 6) || 名同(m, ml, "小写", 6)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = 简_大小写(");
                值名(q->甲);
                fprintf(出, ", %d);\n", 名同(m, ml, "大写", 6));
                return;
            }
            if (名同(m, ml, "去空白", 9)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = 简_去空白(");
                值名(q->甲);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "开头是", 9) || 名同(m, ml, "结尾是", 9)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = %s(", 名同(m, ml, "开头是", 9) ? "简_开头" : "简_结尾");
                值名(q->甲);
                fprintf(出, ", ");
                值名(q->多[0]);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "查找", 6)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = 简_查找(");
                值名(q->甲);
                fprintf(出, ", ");
                值名(q->多[0]);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "替换", 6)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = 简_替换(");
                值名(q->甲);
                fprintf(出, ", ");
                值名(q->多[0]);
                fprintf(出, ", ");
                值名(q->多[1]);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "切分", 6)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = 简_切分(");
                值名(q->甲);
                fprintf(出, ", ");
                值名(q->多[0]);
                fprintf(出, ");\n");
                return;
            }
            if (名同(m, ml, "取", 3)) {
                fprintf(出, "    ");
                值名(果);
                fprintf(出, " = 简_字取(");
                值名(q->甲);
                fprintf(出, ", ");
                值名(q->多[0]);
                fprintf(出, ");\n");
                return;
            }
        }

        fprintf(stderr, "后端还没实现方法：%.*s\n", ml, m);
        exit(4);
    }

    /* ---- 构造结构体 ---- *
     * IR 里 `构造结构` 的 型 就是那个命名类型，值是按字段声明顺序排好的。
     * C 的结构体初始化正好是位置式，一一对上。 */
    case 指_构造结构: {
        const char *ty = C型(q->型);
        字段册 *册 = 找字段册(q->文, q->文长);
        fprintf(出, "    ");
        值名(果);
        fprintf(出, " = (%s){", ty);
        /* 用指定初始化器：`.x = 那个值`。IR 里没给的字段是 -1，直接跳过，
         * 剩下的交给 C 的"未写成员清零"。 */
        int 首 = 1;
        for (int i = 0; i < q->多量; i++) {
            if (q->多[i] < 0) continue;
            if (!首) fputs(", ", 出);
            if (册 && i < 册->数) fprintf(出, ".%.*s = ", 册->字段长[i], 册->字段[i]);
            值名(q->多[i]);
            首 = 0;
        }
        fprintf(出, "};\n");
        return;
    }

    /* ---- 结构体字段 ---- *
     * C 的结构体和简言的结构体布局一致，直接点号访问。
     * 字段名在 IR 的 文 里，不用回查 AST。 */
    case 指_取字段:
        fprintf(出, "    ");
        值名(果);
        fprintf(出, " = ");
        值名(q->甲);
        fprintf(出, ".%.*s;\n", q->文长, q->文 ? q->文 : "");
        return;

    case 指_存字段:
        fprintf(出, "    ");
        值名(q->甲);
        fprintf(出, ".%.*s = ", q->文长, q->文 ? q->文 : "");
        值名(q->乙);
        fprintf(出, ";\n");
        return;

    /* ---- 枚举变体 ---- *
     * 枚举是「标签 + 联合」：变体号一个 int，字段全在 union 里。
     * 拷贝一个枚举值就是拷贝整块，不需要引用计数，也不需要堆。 */
    case 指_构造变体: {
        const char *ty = C型(q->型);
        fprintf(出, "    ");
        值名(果);
        fprintf(出, " = (%s){ .变体 = %s_%.*s", ty, ty, q->文长, q->文 ? q->文 : "");
        if (q->多量 > 0) {
            /* 字段按位置填。AST 里有字段名，但 IR 只留了顺序——
               位置初始化在 C 里同样合法，而且不怕字段改名。 */
            fprintf(出, ", .值.%.*s = { ", q->文长, q->文 ? q->文 : "");
            for (int i = 0; i < q->多量; i++) {
                if (i) fputs(", ", 出);
                值名(q->多[i]);
            }
            fputs(" }", 出);
        }
        fprintf(出, " };\n");
        return;
    }

    case 指_匹配变体: {
        /* 文 里是变体名，前面要补上枚举类型名 */
        const char *ty = C型(值型(f, q->甲));
        fprintf(出, "    ");
        值名(果);
        fprintf(出, " = (");
        值名(q->甲);
        fprintf(出, ".变体 == %s_%.*s);\n", ty, q->文长, q->文 ? q->文 : "");
        return;
    }

    case 指_解构变体:
        /* 文 形如 "圆.半径" */
        fprintf(出, "    ");
        值名(果);
        fprintf(出, " = ");
        值名(q->甲);
        fprintf(出, ".值.%.*s;\n", q->文长, q->文 ? q->文 : "");
        return;

    /* ---- 函数值 ----
     * 简言的函数值在 C 里就是函数指针，没有闭包（匿名函数不抓外层变量），
     * 所以取函数 直接退化成"函数名去掉 &"：函数名本身就当指针用。 */
    case 指_取函数:
        fprintf(出, "    ");
        值名(果);
        fprintf(出, " = ");
        写函数名(q->文, q->文长);
        fprintf(出, ";\n");
        return;

    /* ---- 通道 ---- */
    case 指_建通道: {
        类型 *元 = (q->型 && q->型->种 == 型_通道) ? q->型->元素 : NULL;
        fprintf(出, "    ");
        值名(果);
        if (q->甲 >= 0) {
            fprintf(出, " = 简_通道(");
            值名(q->甲);
            fprintf(出, ", (int)sizeof(%s));\n", C型(元));
        } else {
            fprintf(出, " = 简_通道(8, (int)sizeof(%s));\n", C型(元));
        }
        return;
    }

    case 指_发送:
        fprintf(出, "    简_发(&");
        值名(q->甲);
        fprintf(出, ", &");
        值名(q->乙);
        fprintf(出, ");\n");
        return;

    case 指_接收: {
        类型 *元 = 值型(f, q->甲);
        元 = (元 && 元->种 == 型_通道) ? 元->元素 : NULL;
        fprintf(出, "    { %s _r; 简_收(&", C型(元));
        值名(q->甲);
        fprintf(出, ", &_r); ");
        值名(果);
        fprintf(出, " = _r; }\n");
        return;
    }

    case 指_关通道:
        fprintf(出, "    ");
        值名(q->甲);
        fprintf(出, ".关 = 1; ");
        值名(q->甲);
        fprintf(出, ".n = 0;\n");
        return;

    /* ---- 所有权标记 ----
     * 移动/复制/借用 在编译期已经检查完，运行时什么都不用做。
     * 传值还是传引用，C 的语义和简言在这里刚好一致：结构体按值拷，
     * 组/映射/字符串的堆部分靠各自的堆指针共享。 */
    case 指_移动: case 指_复制: case 指_借用: case 指_可变借用:
        if (果 >= 0) {
            fprintf(出, "    ");
            值名(果);
            fprintf(出, " = ");
            值名(q->甲);
            fprintf(出, ";\n");
        }
        return;

    /* ---- 结果构造 ---- */
    case 指_成功:
    case 指_失败: {
        int 成 = (q->码 == 指_成功);
        fprintf(出, "    ");
        值名(果);
        if (q->甲 >= 0) {
            fprintf(出, " = 简_果造(%d, &", 成);
            值名(q->甲);
            fprintf(出, ", (int)sizeof(");
            值名(q->甲);
            fprintf(出, "));\n");
        } else {
            fprintf(出, " = 简_果造(%d, 0, 0);\n", 成);
        }
        return;
    }

    /* ---- 映射 ---- */
    case 指_构造映射: {
        类型 *kt = (q->型 && q->型->种 == 型_映射) ? q->型->键 : NULL;
        类型 *vt = (q->型 && q->型->种 == 型_映射) ? q->型->值 : NULL;
        const char *ktx = C型(kt), *vtx = C型(vt);
        int 键类 = (kt && kt->种 == 型_浮点) ? 2 : (kt && kt->种 == 型_字符串) ? 3 : 1;

        fprintf(出, "    { 简映 _m%d = {0}; _m%d.键宽 = (int)sizeof(%s); "
                    "_m%d.值宽 = (int)sizeof(%s); _m%d.键类 = %d;\n",
                果, 果, ktx, 果, vtx, 果, 键类);
        for (int i = 0; i + 1 < q->多量; i += 2) {
            fprintf(出, "      { %s _o; 简_映插(&_m%d, &", vtx, 果);
            值名(q->多[i]);
            fprintf(出, ", &");
            值名(q->多[i + 1]);
            fprintf(出, ", &_o); }\n");
        }
        fprintf(出, "      ");
        值名(果);
        fprintf(出, " = _m%d; }\n", 果);
        return;
    }

    /* ---- 遍历 ---- *
     * 遍历开始 造一个迭代器；遍历下一个 取一个元素写进槽里，用完就停。
     * 元素拷出来用 memcpy：这里不知道元素类型，但知道宽度，够了。 */
    case 指_遍历开始:
        fprintf(出, "    ");
        值名(果);
        fprintf(出, ".g = ");
        值名(q->甲);
        fprintf(出, "; ");
        值名(果);
        fprintf(出, ".i = 0;\n");
        return;

    case 指_遍历下一个: {
        /* 甲的迭代器，乙 是循环变量槽。越界时往槽里写零值，
         * 上层用「元素 == 空」判断结束。 */
        int 目 = q->乙;
        fprintf(出, "    if (");
        值名(q->甲);
        fprintf(出, ".i < ");
        值名(q->甲);
        fprintf(出, ".g.n) { memcpy(&");
        值名(目);
        fprintf(出, ", ");
        值名(q->甲);
        fprintf(出, ".g.p + ");
        值名(q->甲);
        fprintf(出, ".i * ");
        值名(q->甲);
        fprintf(出, ".g.元宽, (size_t)");
        值名(q->甲);
        fprintf(出, ".g.元宽); ");
        值名(q->甲);
        fprintf(出, ".i++; }\n    else { memset(&");
        值名(目);
        fprintf(出, ", 0, sizeof(");
        值名(目);
        fprintf(出, ")); }\n");
        return;
    }

    /* ---- 组（动态数组） ---- */
    case 指_构造组: {
        类型 *元 = (q->型 && q->型->种 == 型_组) ? q->型->元素 : NULL;
        const char *ct = C型(元);
        fprintf(出, "    { %s _t%d[] = {", ct, 果);
        if (q->多量 == 0) fputs("0", 出);
        for (int i = 0; i < q->多量; i++) {
            if (i) fputs(", ", 出);
            值名(q->多[i]);
        }
        fprintf(出, "};\n      ");
        值名(果);
        fprintf(出, " = 简_组(_t%d, %d, (int)sizeof(%s)); }\n",
                q->多量 ? 果 : 果, q->多量, ct);
        return;
    }

    case 指_取下标: {
        const char *ct = C型(q->型);
        if (值型(f, q->甲) && 值型(f, q->甲)->种 == 型_映射) {
            fprintf(出, "    { %s _r; 简_映取(&", ct);
            值名(q->甲);
            fprintf(出, ", &");
            值名(q->乙);
            fprintf(出, ", &_r); ");
            值名(果);
            fprintf(出, " = _r; }\n");
            return;
        }
        fprintf(出, "    if (");
        值名(q->乙);
        fprintf(出, " < 0 || ");
        值名(q->乙);
        fprintf(出, " >= ");
        值名(q->甲);
        fprintf(出, ".n) 简_越界(");
        值名(q->乙);
        fprintf(出, ", ");
        值名(q->甲);
        fprintf(出, ".n);\n");

        fprintf(出, "    ");
        值名(果);
        fprintf(出, " = ((");
        fputs(ct, 出);
        fprintf(出, " *)");
        值名(q->甲);
        fprintf(出, ".p)[");
        值名(q->乙);
        fprintf(出, "];\n");
        return;
    }

    case 指_存下标: {
        const char *ct = C型(值型(f, q->丙));
        fprintf(出, "    if (");
        值名(q->乙);
        fprintf(出, " < 0 || ");
        值名(q->乙);
        fprintf(出, " >= ");
        值名(q->甲);
        fprintf(出, ".n) 简_越界(");
        值名(q->乙);
        fprintf(出, ", ");
        值名(q->甲);
        fprintf(出, ".n);\n");

        fprintf(出, "    ((");
        fputs(ct, 出);
        fprintf(出, " *)");
        值名(q->甲);
        fprintf(出, ".p)[");
        值名(q->乙);
        fprintf(出, "] = ");
        值名(q->丙);
        fprintf(出, ";\n");
        return;
    }

    case 指_长度: {
        /* 字符串的 # 是「几个字符」，不是几个字节。
         * 中文在 UTF-8 里占 3 个字节，直接返回 .n 会得到 6 而不是 2。 */
        类型 *t = 值型(f, q->甲);
        fprintf(出, "    ");
        值名(果);
        if (t && t->种 == 型_字符串) {
            fprintf(出, " = 简_字数(");
            值名(q->甲);
            fprintf(出, ");\n");
        } else {
            fprintf(出, " = ");
            值名(q->甲);
            fprintf(出, ".n;\n");
        }
        return;
    }

    /* ---- 类型转换 ---- */
    case 指_转换: {
        类型 *从 = 值型(f, q->甲);
        类型 *到 = q->型;
        int 从种 = 从 ? 从->种 : 型_未知;
        int 到种 = 到 ? 到->种 : 型_未知;

        if (到种 == 型_字符串) {
            fprintf(出, "    ");
            值名(果);
            if (从种 == 型_浮点) fprintf(出, " = 简_浮字(");
            else if (从种 == 型_字符串) fprintf(出, " = (");
            else fprintf(出, " = 简_整字((long long)");
            值名(q->甲);
            fprintf(出, "%s;\n", 从种 == 型_字符串 ? ")" : ")");
            return;
        }
        if (到种 == 型_浮点) {
            fprintf(出, "    ");
            值名(果);
            fprintf(出, " = (double)");
            值名(q->甲);
            fprintf(出, ";\n");
            return;
        }
        if (到种 == 型_布尔) {
            fprintf(出, "    ");
            值名(果);
            fprintf(出, " = (");
            值名(q->甲);
            fprintf(出, " != 0);\n");
            return;
        }
        /* 其余（整数、枚举、命名类型）一律按整数处理 */
        fprintf(出, "    ");
        值名(果);
        fprintf(出, " = (long long)");
        值名(q->甲);
        fprintf(出, ";\n");
        return;
    }

    /* ---- 逻辑与短路：IR 里 如果需要 两个都求值，这里不做短路 ---- */
    default:
        fprintf(stderr, "后端还没实现指令：%s（%s 第 %d 条）\n",
                指名[q->码], "函数", 0);
        exit(4);
    }
}

/* -------------------------------------------------------------- 函数 -- */

static 中间函数 *找函数(const char *名, int 长)
{
    for (中间函数 *f = 模块体.头; f; f = f->下)
        if (f->名长 == 长 && 名 && memcmp(f->名, 名, (size_t)长) == 0) return f;
    return NULL;
}

/* 第 i 个形参是不是借用 */
static int 形参借用(中间函数 *f, int i)
{
    if (!f || !f->源) return 0;
    int k = 0;
    for (int j = 0; j < f->源->子数; j++) {
        节点 *p = f->源->子[j];
        if (p->种类 != 节_参数) continue;
        if (k++ == i) return (p->标志 & (标_借用 | 标_可变借用)) != 0;
    }
    return 0;
}

static int 参数借用位(中间函数 *f, int 参数序)
{
    return 形参借用(f, 参数序);
}

/* 参数就是值表最前面的若干个「变量槽」——降函数时先登记参数，再降函数体，
 * 这个顺序是 IR 层保证的。 */
static int 参数个数(中间函数 *f, int *编号)
{
    int 需 = f->型 && f->型->种 == 型_函数 ? f->型->项数 : 0;
    int 得 = 0;
    for (int i = 0; i < f->值数 && 得 < 需; i++) {
        if (f->值[i].是变量 && f->值[i].名) 编号[得++] = i;
    }
    return 得;
}

static void 写原型(中间函数 *f)
{
    int 编[16]; int 数 = 参数个数(f, 编);
    fprintf(出, "static %s ", C型(f->型 ? f->型->返回 : NULL));
    写函数名(f->名, f->名长);
    fputc('(', 出);
    if (数 == 0) fputs("void", 出);
    for (int i = 0; i < 数; i++) {
        if (i) fputs(", ", 出);
        fprintf(出, "%s %sv%d", C型(值型(f, 编[i])),
                参数借用位(f, i) ? "*" : "", 编[i]);
    }
    fputs(");\n", 出);
}

static void 写函数(中间函数 *f)
{
    int 参编[16]; int 参数 = 参数个数(f, 参编);
    unsigned char *是参 = (unsigned char *)calloc((size_t)(f->值数 + 1), 1);
    for (int i = 0; i < 参数; i++)
        if (参编[i] >= 0 && 参编[i] < f->值数) 是参[参编[i]] = 1;

    fprintf(出, "\n/* 简言函数：");
    fwrite(f->名, 1, (size_t)f->名长, 出);
    fprintf(出, "  （%d 块 / %d 条指令） */\n", f->下个块号, f->指令数);

    fprintf(出, "static %s ", C型(f->型 ? f->型->返回 : NULL));
    写函数名(f->名, f->名长);
    fputc('(', 出);
    if (参数 == 0) fputs("void", 出);
    for (int i = 0; i < 参数; i++) {
        if (i) fputs(", ", 出);
        fprintf(出, "%s %sv%d", C型(值型(f, 参编[i])),
                参数借用位(f, i) ? "*" : "", 参编[i]);
    }
    fputs(")\n{\n", 出);

    /* 把借用形参标出来，后面所有 值名() 都会自动解引用 */
    unsigned char *指针参 = (unsigned char *)calloc((size_t)(f->值数 + 1), 1);
    for (int i = 0; i < 参数; i++)
        if (参数借用位(f, i) && 参编[i] >= 0 && 参编[i] < f->值数) 指针参[参编[i]] = 1;
    当前函 = f; 当前指针参 = 指针参; 当前值数 = f->值数;

    /* 局部变量：除了形参，所有值都声明一遍。
     * 不复用、不合并——gcc -O2 会做掉，我们不必替它操心。 */

    /* 迭代器是唯一一种「类型不在值表里」的值：遍历开始 的结果类型是未知，
     * 但 C 里必须声明成 简遍 才能用。先扫一遍指令，把它们标出来。 */
    unsigned char *是遍 = (unsigned char *)calloc((size_t)(f->值数 + 1), 1);
    for (基本块 *b = f->首块; b; b = b->下)
        for (指令 *q = b->头; q; q = q->下)
            if (q->码 == 指_遍历开始 && q->结果 >= 0 && q->结果 < f->值数)
                是遍[q->结果] = 1;

    int 有声明 = 0;
    for (int i = 0; i < f->值数; i++) {
        if (是参[i]) continue;
        /* 空类型的值（比如 打印 的返回值）在 C 里得有个落脚处，
         * 声明成 long long 而不是 void——void 变量不存在。 */
        类型 *t = 值型(f, i);
        if (t && t->种 == 型_函数) {
            /* 函数指针不能写成 `类型 v0`，要用 C 的括号语法：
               long long (*v0)(long long, double) */
            fprintf(出, "    %s (*v%d)(", C型(t->返回), i);
            if (t->项数 == 0) fputs("void", 出);
            for (int k = 0; k < t->项数; k++) {
                if (k) fputs(", ", 出);
                fputs(C型(t->项[k]), 出);
            }
            fprintf(出, ");");
            if (f->值[i].名 && f->值[i].是变量) {
                fprintf(出, "  /* ");
                fwrite(f->值[i].名, 1, (size_t)f->值[i].名长, 出);
                fprintf(出, " */");
            }
            fputc('\n', 出);
            有声明 = 1;
            continue;
        }
        const char *ty = 是遍[i] ? "简遍"
                       : (t && t->种 == 型_空) ? "long long" : C型(t);
        fprintf(出, "    %s v%d;", ty, i);
        if (f->值[i].名 && f->值[i].是变量) {
            fprintf(出, "  /* ");
            fwrite(f->值[i].名, 1, (size_t)f->值[i].名长, 出);
            fprintf(出, " */");
        }
        fputc('\n', 出);
        有声明 = 1;
    }
    free(是遍);
    free(是参);
    if (有声明) fputc('\n', 出);

    /* 块：按创建顺序输出，靠标签和 goto 连起来。
     * 这样不用分析控制流，也不会被「块顺序 ≠ 执行顺序」坑到。 */
    for (基本块 *b = f->首块; b; b = b->下) {
        fprintf(出, "L%d: ;\n", b->号);
        for (指令 *q = b->头; q; q = q->下) 写指令(f, q);
    }
    fputs("}\n", 出);

    /* 指针表要等整个函数体写完才能释放 */
    free(指针参);
    当前指针参 = NULL; 当前函 = NULL; 当前值数 = 0;
}

/* -------------------------------------------------------------- 模块 -- */

static void 生成C(节点 *根)
{
    行("/* ============================================================");
    行(" *  这份 C 代码由 简言 后端自动生成，请勿手工修改。");
    行(" *  改 .jy 源文件，重新编译。");
    行(" * ============================================================ */");
    行("");

    写运行时();
    写类型定义(根);

    /* 先出全部原型：函数之间可以互相调用（递归、前向引用），
     * 没有原型 gcc 会当成隐式声明，参数类型全错。 */
    行("/* ---- 函数原型 ---- */");
    for (中间函数 *f = 模块体.头; f; f = f->下) 写原型(f);

    for (中间函数 *f = 模块体.头; f; f = f->下) 写函数(f);

    /* 入口：找 主，没有就报错 */
    行("");
    行("/* ---- 入口 ---- */");
    行("int main(void) {");
    int 有主 = 0;
    for (中间函数 *f = 模块体.头; f; f = f->下) {
        if (f->名长 == 3 && memcmp(f->名, "主", 3) == 0) {
            if (f->型 && f->型->返回 && f->型->返回->种 != 型_空) {
                行("    return (int)主();");
            } else {
                行("    主();");
                行("    return 0;");
            }
            有主 = 1;
            break;
        }
    }
    if (!有主) {
        行("    fputs(\"这个程序没有 主() 函数\\n\", stderr);");
        行("    return 1;");
    }
    行("}");
}

/* ---------------------------------------------------------------- 主 -- */

/* 找 lib简言图.a。
 *
 * 这个库是 stb_image / stb_image_write / nanosvg 编出来的静态库，
 * 里面四十多万行第三方代码。生成的 C 只引用它、不含它。
 *
 * 找的顺序：先看 简编译 自己所在的目录（../第三方/ 和同目录），
 * 再看当前工作目录。都没有就退化成"不链"——纯计算程序照样能编，
 * 用到 图 的时候才会在链接阶段报 undefined reference，那时提示里有解法。 */
static const char *找图库(void)
{
    static char 试[1200];
    char 自身[1000];
    ssize_t n = readlink("/proc/self/exe", 自身, sizeof(自身) - 1);
    if (n > 0) {
        自身[n] = 0;
        char *斜 = strrchr(自身, '/');
        if (斜) {
            *斜 = 0;
            const char *候选[] = { "%s/../第三方/lib简言图.a", "%s/lib简言图.a",
                                   "%s/第三方/lib简言图.a" };
            for (int i = 0; i < 3; i++) {
                snprintf(试, sizeof(试), 候选[i], 自身);
                FILE *f = fopen(试, "rb");
                if (f) { fclose(f); return 试; }
            }
        }
    }
    const char *相对 = "第三方/lib简言图.a";
    FILE *f = fopen(相对, "rb");
    if (f) { fclose(f); return 相对; }
    return NULL;
}

static void 用法(void)
{
    fputs(
        "简编译 —— 简言后端（IR → C → 机器码）\n\n"
        "  简编译 文件.jy              生成 a.out\n"
        "  简编译 文件.jy -o 名字      生成指定名字\n"
        "  简编译 文件.jy -c           只出 C 代码（stdout）\n"
        "  简编译 文件.jy -c 文件.c    只出 C 代码（写文件）\n",
        stderr);
}

int main(int argc, char **argv)
{
    if (argc < 2) { 用法(); return 2; }

    const char *路径 = NULL;
    const char *输出 = "a.out";
    const char *C文件 = NULL;
    int 只要C = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) { if (++i < argc) 输出 = argv[i]; }
        else if (strcmp(argv[i], "-c") == 0) {
            只要C = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') C文件 = argv[++i];
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            用法();
            return 0;
        }
        else 路径 = argv[i];
    }
    if (!路径) { 用法(); return 2; }

    建常用类型();
    建关键字表();

    int 源长 = 0;
    char *源 = 读文件(路径, &源长);
    if (!源) { fprintf(stderr, "打不开文件：%s\n", 路径); return 1; }

    int 词数 = 0, 词法错误 = 0;
    词 *词们 = 切好所有词(源, 源长, &词数, &词法错误);

    语法器 P;
    备好语法器(&P, 词们, 词数, 源, 源长, 路径);
    节点 *根 = 解析程序(&P);

    if (!生成(根)) {
        fprintf(stderr, "语义检查没通过，不生成代码\n");
        打印诊断();
        return 1;
    }

    /* ---- 出 C ---- */
    char 临时路径[512];
    if (只要C) {
        出 = C文件 ? fopen(C文件, "w") : stdout;
        if (!出) { fprintf(stderr, "写不了 %s\n", C文件); return 1; }
    } else {
        snprintf(临时路径, sizeof(临时路径), "/tmp/简言_%d.c", (int)getpid());
        出 = fopen(临时路径, "w");
        if (!出) { fprintf(stderr, "写不了临时文件\n"); return 1; }
    }

    生成C(根);

    if (只要C) {
        if (C文件) fclose(出);
        return 0;
    }
    fclose(出);

    /* ---- 交给 gcc ---- */
    /* 图片库排在源文件后面：静态链接按顺序取符号，放前面会一个都不拉。 */
    char 命令[2048];
    const char *图库 = 找图库();
    if (图库)
        snprintf(命令, sizeof(命令),
                 "gcc -O2 -w -o '%s' '%s' '%s' -lm 2>&1", 输出, 临时路径, 图库);
    else
        snprintf(命令, sizeof(命令),
                 "gcc -O2 -w -o '%s' '%s' -lm 2>&1", 输出, 临时路径);

    int rc = system(命令);
    remove(临时路径);
    if (rc != 0) {
        fprintf(stderr, "gcc 编译失败。想看得细一点：\n"
                        "  简编译 %s -c /tmp/x.c && gcc -O2 -o %s /tmp/x.c %s -lm\n",
                路径, 输出, 图库 ? 图库 : "");
        if (!图库)
            fprintf(stderr, "\n提示：用到 `图` 的话需要图形运行时库，"
                            "在 简言/第三方 目录下跑 `make` 生成它。\n");
        return 1;
    }

    return 0;
}
