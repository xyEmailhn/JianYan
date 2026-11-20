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
    行("    fprintf(stderr, \"下标越界：%lld，长度 %lld\\n\", (long long)i, (long long)n);");
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
    行("static 简果 简_果造(int 成, const void *v, int 宽) {");
    行("    简果 g; g.成 = 成; g.宽 = 宽;");
    行("    g.p = (char *)malloc((size_t)(宽 > 0 ? 宽 : 1));");
    行("    if (!g.p) { fputs(\"内存不够了\\n\", stderr); exit(3); }");
    行("    if (宽 > 0 && v) memcpy(g.p, v, (size_t)宽); else memset(g.p, 0, 1);");
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
                fprintf(出, "    { %s _r; memset(&_r, 0, sizeof(_r)); "
                            "memcpy(&_r, ", ct);
                值名(q->甲);
                fprintf(出, ".p, sizeof(_r)); ");
                值名(果);
                fprintf(出, " = _r; }\n");
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
                fprintf(出, "    { %s _r; memset(&_r, 0, sizeof(_r)); "
                            "memcpy(&_r, ", ct);
                值名(q->甲);
                fprintf(出, ".p, sizeof(_r)); ");
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
        fprintf(出, "    ");
        值名(果);
        fprintf(出, " = (%s){", ty);
        for (int i = 0; i < q->多量; i++) {
            if (i) fputs(", ", 出);
            值名(q->多[i]);
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
    char 命令[1024];
    snprintf(命令, sizeof(命令),
             "gcc -O2 -w -o '%s' '%s' -lm 2>&1", 输出, 临时路径);
    int rc = system(命令);
    remove(临时路径);
    if (rc != 0) {
        fprintf(stderr, "gcc 编译失败。想看得细一点：\n"
                        "  简编译 %s -c /tmp/x.c && gcc -O2 -o %s /tmp/x.c -lm\n",
                路径, 输出);
        return 1;
    }

    return 0;
}
