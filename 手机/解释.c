/* ============================================================================
 *  解释.c —— 手机上跑的简言（IR 解释执行）
 *  ----------------------------------------------------------------------------
 *  为什么要有它：手机上没有 gcc，也没有 as/ld，原生后端自出的又是 x86-64。
 *  想让 App 里"点一下就能跑"，只剩一条路——把中间表示直接解释执行。
 *
 *  它不生成任何机器码，只按 IR 的指令一条条算。好处是：
 *    - 与 CPU 无关，arm64 / x86 都能跑；
 *    - 与 C 后端共享同一套前端（词法/语法/语义/IR），诊断信息完全一致；
 *    - 语义跟着 IR 走，不存在"两套后端各算各的"。
 *
 *  代价是慢。这是教学与验证用的执行器，不是性能后端。
 *
 *  这一版支持：整数、浮点、布尔、字符串、函数（含递归）、条件、循环、
 *  打印/读入/退出、类型转换、字符串方法。组/映射/结构/变体/线程/网络
 *  明确报「手机版暂不支持」，不猜、不静默。
 * ========================================================================== */

#define 简_只要中间 1
#include "../中间/ir.c"

/* ---- 值 ---- */
typedef struct { const char *p; long long n; } 解串;

typedef struct 解组 解组;
typedef struct 解映 解映;

typedef struct {
    int 种;
    long long i;        /* 整数、布尔、句柄 */
    double f;           /* 浮点 */
    解串 *字;           /* 字符串 */
    解组 *组;           /* 组 */
    解映 *映;           /* 映射 */
    void *p;            /* 结构、变体、迭代器 */
} 解值;

/* 解释器自己的组：元素是「解值」，不是一个固定宽度的 C 类型。
 * C 后端用 简组（opaque 字节数组 + 元宽），那是为了生成代码方便；
 * 解释器里元素天生异构，按字节搬反而要来回拷。 */
struct 解组 { 解值 *项; long long 数, 容; };

/* 映射：线性查找。C 后端是开放寻址的哈希表，这里图省事用数组——
 * 教学程序的键数通常个位数，O(n) 完全够，而且省掉一套哈希函数。 */
typedef struct { 解值 键, 值; } 解对;
struct 解映 { 解对 *项; long long 数, 容; };

/* 结果类型：和 C 后端的 简果 一个意思（成/载荷），载荷换成 解值。 */
typedef struct { int 成; 解值 v; } 解果;

/* ---- 结构 / 变体 ----
 * 解释器不按 C 的偏移访问字段，而是存一张「名字 → 值」的小表。
 * 慢一点，但换来两件事：不需要复刻 C 后端的布局计算，也不怕字段改名。 */
typedef struct { const char *名; int 名长; 解值 值; } 解字段;
typedef struct { int 数; 解字段 *项; } 解记;
typedef struct { const char *标; int 标长; 解记 记; } 解变;

/* 类型录：类型名（或变体名）→ 字段名顺序。从语法树上摘一次，全局共用。 */
typedef struct {
    const char *名; int 名长;
    int 字段数;
    const char **字段; int *字段长;
} 解型录;
static 解型录 解_录[512];
static int 解_录数 = 0;

static 解型录 *解_找录(const char *名, int 名长)
{
    for (int i = 0; i < 解_录数; i++)
        if (解_录[i].名长 == 名长 && memcmp(解_录[i].名, 名, (size_t)名长) == 0)
            return &解_录[i];
    return NULL;
}

/* 从语法树上摘字段名：结构取自己的字段，枚举的每个变体各记一条。 */
static void 解_建型录(节点 *根)
{
    for (int i = 0; i < 根->子数 && 解_录数 < 511; i++) {
        节点 *n = 根->子[i];
        if (n->种类 == 节_结构) {
            解型录 *e = &解_录[解_录数++];
            e->名 = n->文字; e->名长 = n->文字长;
            e->字段 = (const char **)calloc((size_t)n->子数 + 1, sizeof(char *));
            e->字段长 = (int *)calloc((size_t)n->子数 + 1, sizeof(int));
            for (int k = 0; k < n->子数; k++) {
                节点 *f = n->子[k];
                if (f->种类 != 节_字段) continue;
                e->字段[e->字段数] = f->文字;
                e->字段长[e->字段数] = f->文字长;
                e->字段数++;
            }
        } else if (n->种类 == 节_枚举) {
            for (int k = 0; k < n->子数 && 解_录数 < 511; k++) {
                节点 *v = n->子[k];
                if (v->种类 != 节_变体) continue;
                解型录 *e = &解_录[解_录数++];
                e->名 = v->文字; e->名长 = v->文字长;
                e->字段 = (const char **)calloc((size_t)v->子数 + 1, sizeof(char *));
                e->字段长 = (int *)calloc((size_t)v->子数 + 1, sizeof(int));
                for (int j = 0; j < v->子数; j++) {
                    节点 *f = v->子[j];
                    if (f->种类 != 节_字段) continue;
                    e->字段[e->字段数] = f->文字;
                    e->字段长[e->字段数] = f->文字长;
                    e->字段数++;
                }
            }
        }
    }
}

static 解值 *解_找字段(解记 *r, const char *名, int 名长)
{
    for (int i = 0; i < r->数; i++)
        if (r->项[i].名长 == 名长 && memcmp(r->项[i].名, 名, (size_t)名长) == 0)
            return &r->项[i].值;
    return NULL;
}

static int 解_错 = 0;

/* 手机上必须有一道闸：`当 真 { }` 会把整个 App 卡成一块砖。
 * 上限按「大约两亿条指令」定，桌面跑三五秒，手机十来秒——
 * 够任何教学程序跑完，又不至于让用户以为死机了。 */
static long long 解_步数 = 0;
#define 解_步上限 200000000LL

static void 解死(const char *msg)
{
    fprintf(stderr, "手机版解释器：%s\n", msg);
    解_错 = 1;
}

static 解串 *解_新串(const char *p, long long n);

/* 字符串常量的文是**源码原文**，带引号、带 \n 这类转义——和 C 后端的
 * 写字面量 一个输入。这里做同样的反转义，两边才能得到同一个字符串。 */
static 解串 *解_字面量(const char *s, int n)
{
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') { s++; n -= 2; }
    char *b = (char *)malloc((size_t)n + 1);
    if (!b) { fprintf(stderr, "内存不够\n"); exit(3); }
    long long 长 = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            char e = s[++i];
            switch (e) {
            case 'n':  b[长++] = '\n'; break;
            case 't':  b[长++] = '\t'; break;
            case 'r':  b[长++] = '\r'; break;
            case '0':  b[长++] = '\0'; break;
            case '"':  b[长++] = '"';  break;
            case '\\': b[长++] = '\\'; break;
            default:   b[长++] = e;    break;
            }
        } else {
            b[长++] = s[i];
        }
    }
    b[长] = 0;
    解串 *r = 解_新串(b, 长);
    free(b);
    return r;
}

static 解串 *解_新串(const char *p, long long n)
{
    解串 *s = (解串 *)malloc(sizeof(解串));
    char *q = (char *)malloc((size_t)n + 1);
    if (!s || !q) { fprintf(stderr, "内存不够\n"); exit(3); }
    if (n && p) memcpy(q, p, (size_t)n);
    q[n] = 0;
    s->p = q;
    s->n = n;
    return s;
}

/* UTF-8 字符数：数非续字节 */
static long long 解_字数(const char *p, long long n)
{
    long long c = 0;
    for (long long i = 0; i < n; i++)
        if (((unsigned char)p[i] & 0xC0) != 0x80) c++;
    return c;
}

/* 第 i 个字符的字节起点与长度；越界返回 0 */
static int 解_字符(const 解串 *s, long long i, long long *起, long long *长)
{
    long long n = 0;
    for (long long k = 0; k < s->n; ) {
        long long 步 = 1;
        unsigned char c = (unsigned char)s->p[k];
        if ((c & 0x80) == 0) 步 = 1;
        else if ((c & 0xE0) == 0xC0) 步 = 2;
        else if ((c & 0xF0) == 0xE0) 步 = 3;
        else if ((c & 0xF8) == 0xF0) 步 = 4;
        if (k + 步 > s->n) 步 = s->n - k;
        if (n == i) { *起 = k; *长 = 步; return 1; }
        k += 步;
        n++;
    }
    return 0;
}

static 中间函数 *解_找函数(模块 *m, const char *名, int 名长)
{
    for (中间函数 *f = m->头; f; f = f->下)
        if (f->名长 == 名长 && memcmp(f->名, 名, (size_t)名长) == 0) return f;
    return NULL;
}

static 基本块 *解_找块(中间函数 *f, int 号)
{
    for (基本块 *b = f->首块; b; b = b->下)
        if (b->号 == 号) return b;
    return NULL;
}

/* 值 → 人类可读的一段文字（打印用）。%g 与 C 后端一致。 */
static void 解_写值(解值 *v)
{
    switch (v->种) {
    case 型_字符串:
        if (v->字 && v->字->n) fwrite(v->字->p, 1, (size_t)v->字->n, stdout);
        break;
    case 型_浮点:  printf("%g", v->f); break;
    case 型_布尔:  fputs(v->i ? "真" : "假", stdout); break;
    case 型_空:    break;
    default:       printf("%lld", v->i); break;
    }
}

static void 解_清值(解值 *v)
{
    /* 字符串按值复制（指针共享），与 C 后端一致；
       解释器不在中途释放字符串，交给进程退出一并回收。 */
    v->种 = 型_空;
    v->i = 0;
    v->f = 0;
    v->字 = NULL;
    v->组 = NULL;
    v->映 = NULL;
    v->p = NULL;
}

/* 整数按位宽截断：C 后端用 int8_t/int16_t/int32_t，溢出是静默回绕。
 * 解释器要是拿 long long 一路算下去，`整数8(300)` 就会给出 300 而不是 44。 */
static long long 解_截(long long v, 类型 *t)
{
    if (!t || t->种 != 型_整数) return v;
    switch (t->位宽) {
    case 8:  return (long long)(int8_t)v;
    case 16: return (long long)(int16_t)v;
    case 32: return (long long)(int32_t)v;
    default: return v;
    }
}

static int 解_真(解值 *v)
{
    if (v->种 == 型_浮点) return v->f != 0.0;
    if (v->种 == 型_字符串) return v->字 && v->字->n;
    return v->i != 0;
}

static int 解_是字符串(解值 *v) { return v->种 == 型_字符串; }

/* 字符串比较，返回 -1/0/1 */
static int 解_比串(const 解串 *a, const 解串 *b)
{
    long long n = a->n < b->n ? a->n : b->n;
    int c = n ? memcmp(a->p, b->p, (size_t)n) : 0;
    if (c) return c < 0 ? -1 : 1;
    if (a->n < b->n) return -1;
    if (a->n > b->n) return 1;
    return 0;
}

/* ---- 调用用户函数 ---- */
static int 解_执行(中间函数 *f, 解值 *实参, int 实参数, 解值 *返回);

static int 解_名(const char *a, int an, const char *b)
{
    int bn = (int)strlen(b);
    return an == bn && memcmp(a, b, (size_t)an) == 0;
}

/* ---- 内置 ---- */
static int 解_内置调用(中间函数 *f, 指令 *q, 解值 *槽, 解值 *返)
{
    const char *名 = q->文;
    int 名长 = q->文长;

    /* ---- 数学：直接调 libm ----
     * 精度、边界行为（sqrt(-1)=NaN、atan2(0,0)=0）都跟 C 后端一致，
     * 因为本来就是同一个函数。参数统一按 double 传。 */
    {
        static const struct { const char *名; int 数; } 表[] = {
            {"开方",1},{"正弦",1},{"余弦",1},{"正切",1},{"反正弦",1},{"反余弦",1},
            {"反正切",2},{"指数",1},{"对数",1},{"对数10",1},{"幂",2},{"最小",2},{"最大",2},
            {"向下取整",1},{"向上取整",1},{"四舍五入",1},{"绝对值",1},
            {NULL,0}
        };
        for (int i = 0; 表[i].名; i++) {
            if (!解_名(名, 名长, 表[i].名)) continue;
            double a = q->多量 > 0 ? (槽[q->多[0]].种 == 型_浮点 ? 槽[q->多[0]].f : (double)槽[q->多[0]].i) : 0;
            double b = q->多量 > 1 ? (槽[q->多[1]].种 == 型_浮点 ? 槽[q->多[1]].f : (double)槽[q->多[1]].i) : 0;
            double v;
            switch (i) {
            case 0: v = sqrt(a); break;
            case 1: v = sin(a); break;
            case 2: v = cos(a); break;
            case 3: v = tan(a); break;
            case 4: v = asin(a); break;
            case 5: v = acos(a); break;
            case 6: v = atan2(a, b); break;
            case 7: v = exp(a); break;
            case 8: v = log(a); break;
            case 9: v = log10(a); break;
            case 10: v = pow(a, b); break;
            case 11: v = fmin(a, b); break;
            case 12: v = fmax(a, b); break;
            case 13: v = floor(a); break;
            case 14: v = ceil(a); break;
            case 15: v = round(a); break;
            default: v = fabs(a); break;
            }
            解_清值(返); 返->种 = 型_浮点; 返->f = v;
            return 1;
        }
    }

    if (名长 == 6 && memcmp(名, "打印", 6) == 0) {
        for (int i = 0; i < q->多量; i++) 解_写值(&槽[q->多[i]]);
        解_清值(返); 返->种 = 型_空; 返->i = 0;
        return 1;
    }
    if (名长 == 9 && memcmp(名, "打印行", 9) == 0) {
        for (int i = 0; i < q->多量; i++) 解_写值(&槽[q->多[i]]);
        fputc('\n', stdout);
        解_清值(返); 返->种 = 型_空; 返->i = 0;
        return 1;
    }
    /* 遍历字符串时 IR 会直接发 简_字取(串, i)。它不是语言里的名字，
     * 是后端和 IR 之间的约定，所以不做"用户能否顶掉"的判断。 */
    if (解_名(名, 名长, "简_字取")) {
        解串 *t = q->多量 > 0 ? 槽[q->多[0]].字 : NULL;
        long long 位 = q->多量 > 1 ? 槽[q->多[1]].i : 0;
        解_清值(返); 返->种 = 型_字符串;
        if (t) {
            long long 起 = 0, 长 = 0;
            if (解_字符(t, 位, &起, &长)) 返->字 = 解_新串(t->p + 起, 长);
            else 返->字 = 解_新串("", 0);
        } else 返->字 = 解_新串("", 0);
        return 1;
    }
    if (解_名(名, 名长, "简_字数")) {
        解串 *t = q->多量 > 0 ? 槽[q->多[0]].字 : NULL;
        解_清值(返); 返->种 = 型_整数;
        返->i = t ? 解_字数(t->p, t->n) : 0;
        return 1;
    }
    if (名长 == 6 && memcmp(名, "退出", 6) == 0) {
        int 码 = q->多量 > 0 ? (int)槽[q->多[0]].i : 0;
        fflush(stdout);
        exit(码);
    }
    (void)f;
    return 0;
}



/* ---- 组 ---- */
static 解组 *解_新组(void)
{
    解组 *g = (解组 *)calloc(1, sizeof(解组));
    if (!g) { fprintf(stderr, "内存不够\n"); exit(3); }
    return g;
}

static void 解_组留(解组 *g, long long 需)
{
    if (需 <= g->容) return;
    long long 新容 = g->容 ? g->容 : 4;
    while (新容 < 需) 新容 *= 2;
    解值 *项 = (解值 *)realloc(g->项, (size_t)新容 * sizeof(解值));
    if (!项) { fprintf(stderr, "内存不够\n"); exit(3); }
    g->项 = 项;
    g->容 = 新容;
}

static void 解_组加(解组 *g, 解值 v)
{
    解_组留(g, g->数 + 1);
    g->项[g->数++] = v;
}

/* 按元素的类型比大小。排序、找最大都靠它。 */
static int 解_比(解值 *a, 解值 *b)
{
    if (a->种 == 型_字符串 && b->种 == 型_字符串)
        return 解_比串(a->字, b->字);
    if (a->种 == 型_浮点 || b->种 == 型_浮点) {
        double x = a->种 == 型_浮点 ? a->f : (double)a->i;
        double y = b->种 == 型_浮点 ? b->f : (double)b->i;
        return x < y ? -1 : x > y ? 1 : 0;
    }
    return a->i < b->i ? -1 : a->i > b->i ? 1 : 0;
}

/* 插入排序：手机上跑的组通常几十个元素，够用，也不必传比较函数指针。 */
static void 解_组排(解组 *g)
{
    for (long long i = 1; i < g->数; i++) {
        解值 钥 = g->项[i];
        long long j = i - 1;
        while (j >= 0 && 解_比(&g->项[j], &钥) > 0) {
            g->项[j + 1] = g->项[j];
            j--;
        }
        g->项[j + 1] = 钥;
    }
}

static int 解_组方法(指令 *q, 解值 *槽)
{
    解值 *自 = &槽[q->甲];
    解值 *r = &槽[q->结果];
    const char *m = q->文;
    int ml = q->文长;
    解组 *g = 自->组;
    if (!g) return 0;

    if (解_名(m, ml, "追加")) {
        if (q->多量 > 0) 解_组加(g, 槽[q->多[0]]);
        解_清值(r); r->种 = 型_空;
        return 1;
    }
    if (解_名(m, ml, "插入")) {
        long long 位 = q->多量 > 0 ? 槽[q->多[0]].i : 0;
        if (位 < 0) 位 = 0;
        if (位 > g->数) 位 = g->数;
        解_组留(g, g->数 + 1);
        for (long long i = g->数; i > 位; i--) g->项[i] = g->项[i - 1];
        g->项[位] = q->多量 > 1 ? 槽[q->多[1]] : (解值){0};
        g->数++;
        解_清值(r); r->种 = 型_空;
        return 1;
    }
    if (解_名(m, ml, "包含")) {
        int 有 = 0;
        if (q->多量 > 0) {
            解值 *v = &槽[q->多[0]];
            for (long long i = 0; i < g->数; i++)
                if (解_比(&g->项[i], v) == 0) { 有 = 1; break; }
        }
        解_清值(r); r->种 = 型_布尔; r->i = 有;
        return 1;
    }
    if (解_名(m, ml, "取")) {
        long long 位 = q->多量 > 0 ? 槽[q->多[0]].i : 0;
        解_清值(r);
        if (位 >= 0 && 位 < g->数) *r = g->项[位];
        else r->种 = q->型 ? q->型->种 : 型_整数;
        return 1;
    }
    if (解_名(m, ml, "删除") || 解_名(m, ml, "弹出")) {
        int 尾 = 解_名(m, ml, "弹出");
        long long 位 = 尾 ? g->数 - 1 : (q->多量 > 0 ? 槽[q->多[0]].i : 0);
        解_清值(r);
        if (位 >= 0 && 位 < g->数) {
            *r = 尾 ? g->项[位] : (解值){0};
            if (!尾) r->种 = q->型 ? q->型->种 : 型_整数;
            for (long long i = 位; i + 1 < g->数; i++) g->项[i] = g->项[i + 1];
            g->数--;
        }
        return 1;
    }
    if (解_名(m, ml, "排序")) { 解_组排(g); 解_清值(r); r->种 = 型_空; return 1; }
    if (解_名(m, ml, "反转")) {
        for (long long i = 0, j = g->数 - 1; i < j; i++, j--) {
            解值 t = g->项[i]; g->项[i] = g->项[j]; g->项[j] = t;
        }
        解_清值(r); r->种 = 型_空;
        return 1;
    }
    if (解_名(m, ml, "清空")) { g->数 = 0; 解_清值(r); r->种 = 型_空; return 1; }
    if (解_名(m, ml, "长度")) { 解_清值(r); r->种 = 型_整数; r->i = g->数; return 1; }
    return 0;
}

/* ---- 映射 ---- */
static 解映 *解_新映(void)
{
    解映 *m = (解映 *)calloc(1, sizeof(解映));
    if (!m) { fprintf(stderr, "内存不够\n"); exit(3); }
    return m;
}

static long long 解_映找(解映 *m, 解值 *键)
{
    for (long long i = 0; i < m->数; i++)
        if (解_比(&m->项[i].键, 键) == 0) return i;
    return -1;
}

static void 解_映设(解映 *m, 解值 键, 解值 值, 解值 *旧)
{
    long long i = 解_映找(m, &键);
    if (i >= 0) {
        if (旧) *旧 = m->项[i].值;
        m->项[i].值 = 值;
        return;
    }
    if (m->数 == m->容) {
        long long 新容 = m->容 ? m->容 * 2 : 8;
        解对 *项 = (解对 *)realloc(m->项, (size_t)新容 * sizeof(解对));
        if (!项) { fprintf(stderr, "内存不够\n"); exit(3); }
        m->项 = 项; m->容 = 新容;
    }
    m->项[m->数].键 = 键;
    m->项[m->数].值 = 值;
    m->数++;
    if (旧) { 解_清值(旧); }
}

static int 解_映方法(指令 *q, 解值 *槽)
{
    解值 *自 = &槽[q->甲];
    解值 *r = &槽[q->结果];
    const char *m = q->文;
    int ml = q->文长;
    解映 *映 = 自->映;
    if (!映) return 0;

    if (解_名(m, ml, "插入") || 解_名(m, ml, "设") || 解_名(m, ml, "存")) {
        解值 旧; 解_清值(&旧);
        解_映设(映, 槽[q->多[0]], 槽[q->多[1]], 解_名(m, ml, "插入") ? &旧 : NULL);
        解_清值(r);
        if (解_名(m, ml, "插入")) { *r = 旧; r->种 = q->型 ? q->型->种 : 型_整数; }
        else r->种 = 型_空;
        return 1;
    }
    if (解_名(m, ml, "取")) {
        long long i = 解_映找(映, &槽[q->多[0]]);
        解_清值(r);
        if (i >= 0) *r = 映->项[i].值;
        else r->种 = q->型 ? q->型->种 : 型_整数;
        return 1;
    }
    if (解_名(m, ml, "包含")) {
        解_清值(r); r->种 = 型_布尔;
        r->i = 解_映找(映, &槽[q->多[0]]) >= 0;
        return 1;
    }
    if (解_名(m, ml, "删除")) {
        long long i = 解_映找(映, &槽[q->多[0]]);
        解_清值(r);
        if (i >= 0) {
            for (long long k = i; k + 1 < 映->数; k++) 映->项[k] = 映->项[k + 1];
            映->数--;
        }
        r->种 = 型_布尔; r->i = i >= 0;
        return 1;
    }
    if (解_名(m, ml, "键组") || 解_名(m, ml, "值组")) {
        int 键 = 解_名(m, ml, "键组");
        解组 *g = 解_新组();
        for (long long i = 0; i < 映->数; i++) 解_组加(g, 键 ? 映->项[i].键 : 映->项[i].值);
        解_清值(r); r->种 = 型_组; r->组 = g;
        return 1;
    }
    if (解_名(m, ml, "清空")) { 映->数 = 0; 解_清值(r); r->种 = 型_空; return 1; }
    if (解_名(m, ml, "长度")) { 解_清值(r); r->种 = 型_整数; r->i = 映->数; return 1; }
    return 0;
}

/* ---- 字符串方法 ---- */

static int 解_含(const 解串 *a, const 解串 *b)
{
    if (b->n <= 0) return 1;
    if (b->n > a->n) return 0;
    for (long long i = 0; i + b->n <= a->n; i++)
        if (memcmp(a->p + i, b->p, (size_t)b->n) == 0) return 1;
    return 0;
}

static 解串 *解_切(const 解串 *s, long long 起, long long 止)
{
    if (起 < 0) 起 = 0;
    if (止 > s->n) 止 = s->n;
    if (止 < 起) 止 = 起;
    return 解_新串(s->p + 起, 止 - 起);
}

/* 返回 1 表示这条方法接了；0 表示不认识（交给上层报错）。 */
static int 解_方法(指令 *q, 解值 *槽)
{
    解值 *自 = &槽[q->甲];
    解值 *r = &槽[q->结果];
    const char *m = q->文;
    int ml = q->文长;

    if (自->种 == 型_组 && 自->组) return 解_组方法(q, 槽);
    if (自->种 == 型_映射 && 自->映) return 解_映方法(q, 槽);

    if (自->种 != 型_组 && 自->种 != 型_字符串 &&
        (解_名(m, ml, "取值") || 解_名(m, ml, "错误值") || 解_名(m, ml, "或默认"))) {
        if (!自->p) {
            /* 可空值没有载荷：或默认 直接判零，取值 就是它自己 */
            解_清值(r);
            if (解_名(m, ml, "或默认")) {
                int 有 = 自->种 == 型_字符串 ? (自->字 && 自->字->n > 0)
                        : 自->种 == 型_浮点 ? 自->f != 0.0 : 自->i != 0;
                *r = 有 ? *自 : (q->多量 > 0 ? 槽[q->多[0]] : (解值){0});
            } else {
                *r = *自;
            }
            return 1;
        }
    }
    if (自->p && (解_名(m, ml, "取值") || 解_名(m, ml, "错误值") || 解_名(m, ml, "或默认"))) {
        解果 *g = (解果 *)自->p;
        解_清值(r);
        if (解_名(m, ml, "或默认")) {
            *r = g->成 ? g->v : (q->多量 > 0 ? 槽[q->多[0]] : (解值){0});
        } else {
            *r = g->v;
        }
        return 1;
    }

    if (自->种 == 型_字符串 && 自->字) {
        解串 *s = 自->字;
        if (解_名(m, ml, "长度"))   { 解_清值(r); r->种 = 型_整数; r->i = 解_字数(s->p, s->n); return 1; }
        if (解_名(m, ml, "字节长度")) { 解_清值(r); r->种 = 型_整数; r->i = s->n; return 1; }
        if (解_名(m, ml, "包含")) {
            解值 *a = &槽[q->多[0]];
            解_清值(r); r->种 = 型_布尔;
            r->i = a->字 ? 解_含(s, a->字) : 1;
            return 1;
        }
        if (解_名(m, ml, "大写") || 解_名(m, ml, "小写")) {
            int 上 = 解_名(m, ml, "大写");
            解串 *t = 解_新串(s->p, s->n);
            char *p = (char *)t->p;
            for (long long i = 0; i < s->n; i++) {
                char c = p[i];
                if (上 && c >= 'a' && c <= 'z') c = (char)(c - 32);
                if (!上 && c >= 'A' && c <= 'Z') c = (char)(c + 32);
                p[i] = c;
            }
            解_清值(r); r->种 = 型_字符串; r->字 = t;
            return 1;
        }
        if (解_名(m, ml, "去空白")) {
            long long a = 0, b = s->n;
            while (a < b && (s->p[a] == ' ' || s->p[a] == '\t' || s->p[a] == '\n' || s->p[a] == '\r')) a++;
            while (b > a && (s->p[b-1] == ' ' || s->p[b-1] == '\t' || s->p[b-1] == '\n' || s->p[b-1] == '\r')) b--;
            解_清值(r); r->种 = 型_字符串; r->字 = 解_新串(s->p + a, b - a);
            return 1;
        }
        if (解_名(m, ml, "开头是") || 解_名(m, ml, "结尾是")) {
            解值 *a = &槽[q->多[0]];
            解串 *t = a->字 ? a->字 : NULL;
            int 好 = 0;
            if (!t) 好 = 1;
            else if (解_名(m, ml, "开头是")) 好 = t->n <= s->n && (t->n == 0 || memcmp(s->p, t->p, (size_t)t->n) == 0);
            else 好 = t->n <= s->n && (t->n == 0 || memcmp(s->p + s->n - t->n, t->p, (size_t)t->n) == 0);
            解_清值(r); r->种 = 型_布尔; r->i = 好;
            return 1;
        }
        if (解_名(m, ml, "查找")) {
            解值 *a = &槽[q->多[0]];
            解串 *t = a->字 ? a->字 : NULL;
            long long 位 = -1;
            if (t && t->n > 0 && t->n <= s->n) {
                for (long long i = 0; i + t->n <= s->n; i++)
                    if (memcmp(s->p + i, t->p, (size_t)t->n) == 0) { 位 = i; break; }
            }
            解_清值(r); r->种 = 型_整数; r->i = 位;
            return 1;
        }
        if (解_名(m, ml, "替换")) {
            解值 *oa = &槽[q->多[0]], *ob = &槽[q->多[1]];
            解串 *旧 = oa->字, *新 = ob->字;
            if (!旧 || 旧->n <= 0) { 解_清值(r); r->种 = 型_字符串; r->字 = s; return 1; }
            long long 数 = 0;
            for (long long i = 0; i + 旧->n <= s->n; ) {
                if (memcmp(s->p + i, 旧->p, (size_t)旧->n) == 0) { 数++; i += 旧->n; }
                else i++;
            }
            long long 新长 = 新 ? 新->n : 0;
            long long 长 = s->n + 数 * (新长 - 旧->n);
            解串 *t = 解_新串(NULL, 长);
            char *p = (char *)t->p;
            long long o = 0, i = 0;
            while (i < s->n) {
                if (i + 旧->n <= s->n && memcmp(s->p + i, 旧->p, (size_t)旧->n) == 0) {
                    if (新长) memcpy(p + o, 新->p, (size_t)新长);
                    o += 新长; i += 旧->n;
                } else p[o++] = s->p[i++];
            }
            p[o] = 0;
            解_清值(r); r->种 = 型_字符串; r->字 = t;
            return 1;
        }
        return 0;
    }

    /* 数字 → 文字。语义层登记了「转字符串」，后端一直靠 简_整字 / 简_浮字。 */
    if (解_名(m, ml, "转字符串")) {
        char b[64];
        解_清值(r); r->种 = 型_字符串;
        if (自->种 == 型_浮点) { snprintf(b, sizeof(b), "%g", 自->f); }
        else { snprintf(b, sizeof(b), "%lld", 自->i); }
        r->字 = 解_新串(b, (long long)strlen(b));
        return 1;
    }
    return 0;
}

/* ---- 主执行循环 ---- */
static int 解_执行(中间函数 *f, 解值 *实参, int 实参数, 解值 *返回)
{
    解值 *槽 = (解值 *)calloc((size_t)(f->值数 + 1), sizeof(解值));
    if (!槽) { fprintf(stderr, "内存不够\n"); exit(3); }

    /* 形参：值表最前面的若干个变量槽，顺序与声明一致 */
    int 参编[16];
    int 需 = f->型 && f->型->种 == 型_函数 ? f->型->项数 : 0;
    int 得 = 0;
    for (int i = 0; i < f->值数 && 得 < 需; i++)
        if (f->值[i].是变量 && f->值[i].名) 参编[得++] = i;
    for (int i = 0; i < 得 && i < 实参数; i++) 槽[参编[i]] = 实参[i];

    基本块 *块 = f->首块;
    解值 结果;
    解_清值(&结果);

    while (块) {
        基本块 *下一块 = NULL;
        for (指令 *q = 块->头; q; q = q->下) {
            if (++解_步数 > 解_步上限) {
                fprintf(stderr, "\n运行太久了，已经停下。是不是死循环？\n");
                fflush(stderr);
                exit(124);
            }
            switch (q->码) {
            case 指_常量: {
                解值 *r = &槽[q->结果];
                解_清值(r);
                if (!q->型) break;
                r->种 = q->型->种;
                if (q->型->种 == 型_字符串) {
                    int 是空 = (q->文 && q->文长 == 3 && memcmp(q->文, "空", 3) == 0);
                    r->字 = 是空 ? 解_新串("", 0)
                                : 解_字面量(q->文 ? q->文 : "", q->文长);
                } else if (q->型->种 == 型_浮点) {
                    char b[80]; int n = q->文长 < 79 ? q->文长 : 0;
                    if (n) { memcpy(b, q->文, (size_t)n); b[n] = 0; r->f = strtod(b, NULL); }
                    r->i = 0;
                } else if (q->型->种 == 型_布尔) {
                    r->i = (q->文 && q->文长 == 3 && memcmp(q->文, "真", 3) == 0)
                         ? 1 : (q->文 && q->文[0] == '1');
                } else if (q->型->种 == 型_整数) {
                    char b[80]; int n = q->文长 < 79 ? q->文长 : 0;
                    if (n) { memcpy(b, q->文, (size_t)n); b[n] = 0; r->i = strtoll(b, NULL, 0); }
                    r->i = 解_截(r->i, q->型);
                } else if (q->型->种 == 型_命名) {
                    /* 结构/枚举的零值。必须先把字段表建出来——
                     * 否则后面 `r.左下 = ...` 找不到落点，报「没有字段」。 */
                    解型录 *e = 解_找录(q->型->名, q->型->名长);
                    解记 *记 = (解记 *)calloc(1, sizeof(解记));
                    if (e) {
                        记->数 = e->字段数;
                        记->项 = (解字段 *)calloc((size_t)e->字段数 + 1, sizeof(解字段));
                        for (int i = 0; i < e->字段数; i++) {
                            记->项[i].名 = e->字段[i];
                            记->项[i].名长 = e->字段长[i];
                        }
                    }
                    r->p = 记;
                } else {
                    r->i = 0;
                }
                break;
            }
            case 指_载:
                槽[q->结果] = 槽[q->甲];
                break;
            case 指_存:
                槽[q->甲] = 槽[q->乙];
                break;

            case 指_加: case 指_减: case 指_乘: case 指_除:
            case 指_整除: case 指_取余:
            case 指_位与: case 指_位或: case 指_位异或:
            case 指_左移: case 指_右移: {
                解值 *a = &槽[q->甲], *b = &槽[q->乙], *r = &槽[q->结果];
                解_清值(r);
                if (q->码 == 指_加 && (解_是字符串(a) || 解_是字符串(b))) {
                    /* 拼接：`..` 与 `+` 在字符串上都走这里 */
                    解串 *x = 解_是字符串(a) ? a->字 : NULL;
                    解串 *y = 解_是字符串(b) ? b->字 : NULL;
                    long long n1 = x ? x->n : 0, n2 = y ? y->n : 0;
                    解串 *s = 解_新串(NULL, n1 + n2);
                    if (n1) memcpy((char *)s->p, x->p, (size_t)n1);
                    if (n2) memcpy((char *)s->p + n1, y->p, (size_t)n2);
                    ((char *)s->p)[n1 + n2] = 0;
                    r->种 = 型_字符串; r->字 = s;
                    break;
                }
                if (a->种 == 型_浮点 || b->种 == 型_浮点) {
                    double x = a->种 == 型_浮点 ? a->f : (double)a->i;
                    double y = b->种 == 型_浮点 ? b->f : (double)b->i;
                    r->种 = 型_浮点;
                    switch (q->码) {
                    case 指_加: r->f = x + y; break;
                    case 指_减: r->f = x - y; break;
                    case 指_乘: r->f = x * y; break;
                    case 指_除: r->f = x / y; break;
                    default: 解死("浮点不支持整除/取余/位运算"); r->f = 0;
                    }
                    break;
                }
                long long x = a->i, y = b->i;
                r->种 = 型_整数;
                类型 *rt = q->型;
                switch (q->码) {
                case 指_加: r->i = x + y; break;
                case 指_减: r->i = x - y; break;
                case 指_乘: r->i = x * y; break;
                case 指_除: r->i = y ? x / y : 0; break;
                case 指_整除: r->i = y ? x / y : 0; break;
                case 指_取余: r->i = y ? x % y : 0; break;
                case 指_位与: r->i = x & y; break;
                case 指_位或: r->i = x | y; break;
                case 指_位异或: r->i = x ^ y; break;
                case 指_左移: r->i = (long long)((unsigned long long)x << y); break;
                case 指_右移: r->i = x >> y; break;
                default: break;
                }
                r->i = 解_截(r->i, rt);
                break;
            }

            case 指_等于: case 指_不等:
            case 指_小于: case 指_大于: case 指_小于等: case 指_大于等: {
                解值 *a = &槽[q->甲], *b = &槽[q->乙], *r = &槽[q->结果];
                解_清值(r);
                r->种 = 型_布尔;
                if (解_是字符串(a) && 解_是字符串(b)) {
                    int c = 解_比串(a->字, b->字);
                    switch (q->码) {
                    case 指_等于: r->i = c == 0; break;
                    case 指_不等: r->i = c != 0; break;
                    case 指_小于: r->i = c < 0; break;
                    case 指_大于: r->i = c > 0; break;
                    case 指_小于等: r->i = c <= 0; break;
                    default: r->i = c >= 0; break;
                    }
                    break;
                }
                {
                    double x = a->种 == 型_浮点 ? a->f : (double)a->i;
                    double y = b->种 == 型_浮点 ? b->f : (double)b->i;
                    switch (q->码) {
                    case 指_等于: r->i = x == y; break;
                    case 指_不等: r->i = x != y; break;
                    case 指_小于: r->i = x < y; break;
                    case 指_大于: r->i = x > y; break;
                    case 指_小于等: r->i = x <= y; break;
                    default: r->i = x >= y; break;
                    }
                }
                break;
            }

            case 指_非: {
                解_清值(&槽[q->结果]);
                槽[q->结果].种 = 型_布尔;
                槽[q->结果].i = !解_真(&槽[q->甲]);
                break;
            }
            case 指_负: {
                解值 *a = &槽[q->甲], *r = &槽[q->结果];
                解_清值(r);
                r->种 = a->种;
                if (a->种 == 型_浮点) r->f = -a->f; else r->i = -a->i;
                break;
            }
            case 指_取反: {
                解_清值(&槽[q->结果]);
                槽[q->结果].种 = 型_整数;
                槽[q->结果].i = ~槽[q->甲].i;
                break;
            }
            case 指_拼接: {
                解值 *a = &槽[q->甲], *b = &槽[q->乙], *r = &槽[q->结果];
                解串 *x = a->字, *y = b->字;
                long long n1 = x ? x->n : 0, n2 = y ? y->n : 0;
                解串 *s = 解_新串(NULL, n1 + n2);
                if (n1) memcpy((char *)s->p, x->p, (size_t)n1);
                if (n2) memcpy((char *)s->p + n1, y->p, (size_t)n2);
                ((char *)s->p)[n1 + n2] = 0;
                解_清值(r); r->种 = 型_字符串; r->字 = s;
                break;
            }

            case 指_转换: {
                解值 *a = &槽[q->甲], *r = &槽[q->结果];
                类型 *到 = q->型;
                int 到种 = 到 ? 到->种 : 型_整数;
                解_清值(r);
                r->种 = 到种;
                if (到种 == 型_字符串) {
                    char b[64];
                    if (a->种 == 型_浮点) { snprintf(b, sizeof(b), "%g", a->f); r->字 = 解_新串(b, (long long)strlen(b)); }
                    else if (a->种 == 型_布尔) { const char *z = a->i ? "真" : "假"; r->字 = 解_新串(z, (long long)strlen(z)); }
                    else { snprintf(b, sizeof(b), "%lld", a->i); r->字 = 解_新串(b, (long long)strlen(b)); }
                } else if (到种 == 型_浮点) {
                    r->f = a->种 == 型_浮点 ? a->f : (double)a->i;
                } else if (到种 == 型_布尔) {
                    r->i = 解_真(a);
                } else {
                    r->i = a->种 == 型_浮点 ? (long long)a->f : a->i;
                    r->i = 解_截(r->i, 到);
                }
                break;
            }

            case 指_构造结构: {
                解型录 *e = 解_找录(q->文, q->文长);
                解记 *r = (解记 *)calloc(1, sizeof(解记));
                r->数 = e ? e->字段数 : q->多量;
                r->项 = (解字段 *)calloc((size_t)r->数 + 1, sizeof(解字段));
                for (int i = 0; i < r->数; i++) {
                    if (e) { r->项[i].名 = e->字段[i]; r->项[i].名长 = e->字段长[i]; }
                    解_清值(&r->项[i].值);
                    if (i < q->多量 && q->多[i] >= 0) r->项[i].值 = 槽[q->多[i]];
                }
                解_清值(&槽[q->结果]);
                槽[q->结果].种 = q->型 ? q->型->种 : 型_命名;
                槽[q->结果].p = r;
                break;
            }

            case 指_取字段: {
                解值 *a = &槽[q->甲], *r = &槽[q->结果];
                解记 *记 = (解记 *)a->p;
                解值 *f = 记 ? 解_找字段(记, q->文, q->文长) : NULL;
                解_清值(r);
                if (f && f->种 != 型_空) *r = *f;
                else r->种 = q->型 ? q->型->种 : 型_整数;
                break;
            }

            case 指_存字段: {
                解值 *a = &槽[q->甲];
                解记 *记 = (解记 *)a->p;
                解值 *f = 记 ? 解_找字段(记, q->文, q->文长) : NULL;
                if (f) *f = 槽[q->乙];
                else { fprintf(stderr, "手机版：结构里没有字段 %.*s\n", q->文长, q->文); 解_错 = 1; }
                break;
            }

            case 指_构造变体: {
                解型录 *e = 解_找录(q->文, q->文长);
                解变 *v = (解变 *)calloc(1, sizeof(解变));
                v->标 = q->文; v->标长 = q->文长;
                v->记.数 = e ? e->字段数 : q->多量;
                v->记.项 = (解字段 *)calloc((size_t)v->记.数 + 1, sizeof(解字段));
                for (int i = 0; i < v->记.数; i++) {
                    if (e) { v->记.项[i].名 = e->字段[i]; v->记.项[i].名长 = e->字段长[i]; }
                    解_清值(&v->记.项[i].值);
                    if (i < q->多量 && q->多[i] >= 0) v->记.项[i].值 = 槽[q->多[i]];
                }
                解_清值(&槽[q->结果]);
                槽[q->结果].种 = q->型 ? q->型->种 : 型_命名;
                槽[q->结果].p = v;
                break;
            }

            case 指_匹配变体: {
                解变 *v = (解变 *)槽[q->甲].p;
                解_清值(&槽[q->结果]);
                槽[q->结果].种 = 型_布尔;
                槽[q->结果].i = v && v->标长 == q->文长 &&
                                memcmp(v->标, q->文 ? q->文 : "", (size_t)q->文长) == 0;
                break;
            }

            case 指_解构变体: {
                /* 文 形如 "圆.半径"：点后面是字段名 */
                解变 *v = (解变 *)槽[q->甲].p;
                const char *点 = q->文 ? memchr(q->文, '.', (size_t)q->文长) : NULL;
                解值 *f = NULL;
                if (v && 点) {
                    int 偏 = (int)(点 - q->文) + 1;
                    f = 解_找字段(&v->记, 点 + 1, q->文长 - 偏);
                }
                解_清值(&槽[q->结果]);
                if (f) 槽[q->结果] = *f;
                else 槽[q->结果].种 = q->型 ? q->型->种 : 型_整数;
                break;
            }

            case 指_成功: case 指_失败: {
                解果 *g = (解果 *)calloc(1, sizeof(解果));
                if (!g) { fprintf(stderr, "内存不够\n"); exit(3); }
                g->成 = (q->码 == 指_成功);
                if (q->甲 >= 0) g->v = 槽[q->甲];
                解_清值(&槽[q->结果]);
                槽[q->结果].种 = q->型 ? q->型->种 : 型_命名;
                槽[q->结果].p = g;
                break;
            }

            case 指_取属性: {
                解值 *a = &槽[q->甲], *r = &槽[q->结果];
                解果 *g = (解果 *)a->p;
                if (a->p && 解_名(q->文, q->文长, "是成功")) {
                    解_清值(r); r->种 = 型_布尔; r->i = g->成;
                    break;
                }
                if (a->p && 解_名(q->文, q->文长, "是失败")) {
                    解_清值(r); r->种 = 型_布尔; r->i = !g->成;
                    break;
                }
                if (解_名(q->文, q->文长, "有值")) {
                    /* 可空：字符串看长度，其余看是不是零 */
                    解_清值(r); r->种 = 型_布尔;
                    if (a->种 == 型_字符串) r->i = a->字 && a->字->n > 0;
                    else if (a->种 == 型_浮点) r->i = a->f != 0.0;
                    else r->i = a->i != 0;
                    break;
                }
                if (a->p && (解_名(q->文, q->文长, "错误") || 解_名(q->文, q->文长, "错误值"))) {
                    解_清值(r); r->种 = q->型 ? q->型->种 : 型_字符串;
                    if (!g->成) *r = g->v;
                    break;
                }
                fprintf(stderr, "手机版暂不支持这个属性：%.*s\n", q->文长, q->文);
                解_错 = 1;
                break;
            }

            case 指_构造映射: {
                解映 *m = 解_新映();
                for (int i = 0; i + 1 < q->多量; i += 2)
                    解_映设(m, 槽[q->多[i]], 槽[q->多[i + 1]], NULL);
                解_清值(&槽[q->结果]);
                槽[q->结果].种 = q->型 ? q->型->种 : 型_映射;
                槽[q->结果].映 = m;
                break;
            }

            case 指_构造组: {
                解组 *g = 解_新组();
                for (int i = 0; i < q->多量; i++) 解_组加(g, 槽[q->多[i]]);
                解_清值(&槽[q->结果]);
                槽[q->结果].种 = q->型 ? q->型->种 : 型_组;
                槽[q->结果].组 = g;
                break;
            }

            case 指_取下标: {
                解值 *a = &槽[q->甲], *b = &槽[q->乙], *r = &槽[q->结果];
                if (a->种 == 型_字符串 && a->字) {
                    long long 位 = b->i, 起 = 0, 长 = 0;
                    解_清值(r); r->种 = 型_字符串;
                    if (解_字符(a->字, 位, &起, &长)) r->字 = 解_新串(a->字->p + 起, 长);
                    else r->字 = 解_新串("", 0);
                    break;
                }
                if (a->种 == 型_组 && a->组) {
                    解_清值(r);
                    if (b->i >= 0 && b->i < a->组->数) *r = a->组->项[b->i];
                    else r->种 = q->型 ? q->型->种 : 型_整数;
                    break;
                }
                if (a->种 == 型_映射 && a->映) {
                    long long i = 解_映找(a->映, b);
                    解_清值(r);
                    if (i >= 0) *r = a->映->项[i].值;
                    else r->种 = q->型 ? q->型->种 : 型_整数;
                    break;
                }
                fprintf(stderr, "手机版暂不支持这种取下标\n");
                解_错 = 1;
                break;
            }

            case 指_存下标: {
                解值 *a = &槽[q->甲];
                if (a->种 == 型_组 && a->组) {
                    long long 位 = 槽[q->乙].i;
                    if (位 >= 0 && 位 < a->组->数) a->组->项[位] = 槽[q->丙];
                    else fprintf(stderr, "下标越界：%lld\n", 位);
                    break;
                }
                if (a->种 == 型_映射 && a->映) {
                    解_映设(a->映, 槽[q->乙], 槽[q->丙], NULL);
                    break;
                }
                fprintf(stderr, "手机版暂不支持这种存下标\n");
                解_错 = 1;
                break;
            }

            case 指_借用: case 指_可变借用:
            case 指_移动: case 指_复制:
                槽[q->结果] = 槽[q->甲];
                break;

            case 指_取函数: {
                /* 函数值就是名字。简言没有闭包（匿名函数不抓外层变量），
                 * 所以一个名字足以定位到降级后的顶层函数。 */
                解_清值(&槽[q->结果]);
                槽[q->结果].种 = 型_函数;
                槽[q->结果].字 = 解_新串(q->文 ? q->文 : "", q->文长);
                break;
            }

            case 指_调用方法: {
                if (!解_方法(q, 槽)) {
                    fprintf(stderr, "手机版暂不支持这个方法：%.*s\n", q->文长, q->文);
                    解_错 = 1;
                }
                break;
            }

            case 指_长度: {
                解值 *a = &槽[q->甲], *r = &槽[q->结果];
                解_清值(r); r->种 = 型_整数;
                if (a->种 == 型_字符串 && a->字) r->i = 解_字数(a->字->p, a->字->n);
                else if (a->种 == 型_组 && a->组) r->i = a->组->数;
                else if (a->种 == 型_映射 && a->映) r->i = a->映->数;
                break;
            }

            case 指_跳: {
                下一块 = 解_找块(f, q->甲);
                goto 换块;
            }
            case 指_条件跳: {
                int 真 = 解_真(&槽[q->甲]);
                下一块 = 解_找块(f, 真 ? q->乙 : q->丙);
                goto 换块;
            }
            case 指_返回: {
                if (q->甲 >= 0) 结果 = 槽[q->甲];
                else 解_清值(&结果);
                free(槽);
                *返回 = 结果;
                return 0;
            }
            case 指_空操作:
            case 指_丢弃:
                break;

            case 指_调用: {
                /* 文 为空 = 调的是函数值，名字在操作数甲里 */
                const char *名 = q->文;
                int 名长 = q->文长;
                if (!名) {
                    解串 *t = 槽[q->甲].字;
                    名 = t ? t->p : "";
                    名长 = t ? (int)t->n : 0;
                }
                /* 用户函数优先（用户可顶掉内置） */
                中间函数 *g = 解_找函数(&模块体, 名, 名长);
                if (!g) {
                    if (解_内置调用(f, q, 槽, &槽[q->结果 >= 0 ? q->结果 : 0])) break;
                    fprintf(stderr, "手机版暂不支持这个内置函数：%.*s\n", 名长, 名);
                    解_错 = 1;
                    break;
                }
                解值 局参[16];
                int n = q->多量 < 16 ? q->多量 : 16;
                for (int i = 0; i < n; i++) 局参[i] = 槽[q->多[i]];
                解值 局返; 解_清值(&局返);
                解_执行(g, 局参, n, &局返);
                if (q->结果 >= 0) 槽[q->结果] = 局返;
                break;
            }

            default:
                fprintf(stderr, "手机版暂不支持这条指令：%s\n",
                        指名[q->码] ? 指名[q->码] : "?");
                解_错 = 1;
                break;
            }
            if (解_错) { free(槽); 解_清值(&结果); *返回 = 结果; return 1; }
        }
    换块:
        块 = 下一块;
    }

    free(槽);
    *返回 = 结果;
    return 0;
}


/* ---- 引入 ----
 * 与 C 后端同一套规则：`引入 工具.数` → 本文件同目录下的 工具/数.jy，
 * 递归展开、按路径去重、不隔离命名空间。C 后端那段在 简编译.c 里，
 * 这里没法复用（那个文件有自己的 main），所以照抄一遍。
 * 抄的是四十行，改的是 two 个地方：exit 换成了错误标志。 */
static char *解_已装[64];
static int 解_装数 = 0;

static 节点 *解_解析一份(const char *路径)
{
    int 长 = 0;
    char *源 = 读文件(路径, &长);
    if (!源) return NULL;
    int 词数 = 0, 词法错误 = 0;
    词 *词们 = 切好所有词(源, 长, &词数, &词法错误);
    语法器 P;
    备好语法器(&P, 词们, 词数, 源, 长, 路径);
    return 解析程序(&P);
}

static void 解_装引入(节点 *根, const char *本文件)
{
    int 原有 = 根->子数;
    for (int i = 0; i < 原有; i++) {
        节点 *n = 根->子[i];
        if (n->种类 != 节_引入) continue;
        if (n->子数 > 0) {
            fprintf(stderr, "%s:%d: `引入 … 为 别名` 和 `引入 …{甲, 乙}` 这一版还没接\n",
                    本文件, n->行);
            解_错 = 1;
            return;
        }
        char 文件[1024];
        int 位 = 0;
        for (int k = (int)strlen(本文件) - 1; k >= 0; k--)
            if (本文件[k] == '/') { 位 = k + 1; break; }
        if (位 + n->文字长 + 4 >= (int)sizeof(文件)) {
            fprintf(stderr, "%s:%d: 引入路径太长\n", 本文件, n->行);
            解_错 = 1;
            return;
        }
        memcpy(文件, 本文件, (size_t)位);
        for (int k = 0; k < n->文字长; k++)
            文件[位 + k] = (n->文字[k] == '.') ? '/' : n->文字[k];
        位 += n->文字长;
        文件[位++] = '.'; 文件[位++] = 'j'; 文件[位++] = 'y'; 文件[位] = 0;

        int 装过 = 0;
        for (int k = 0; k < 解_装数; k++)
            if (strcmp(解_已装[k], 文件) == 0) 装过 = 1;
        if (装过) continue;
        if (解_装数 < 64) 解_已装[解_装数++] = strdup(文件);

        节点 *子根 = 解_解析一份(文件);
        if (!子根) {
            fprintf(stderr, "%s:%d: 引入 `%.*s`，找不到文件 %s\n",
                    本文件, n->行, n->文字长, n->文字, 文件);
            解_错 = 1;
            return;
        }
        解_装引入(子根, 文件);
        if (解_错) return;
        for (int k = 0; k < 子根->子数; k++) {
            节点 *c = 子根->子[k];
            if (c->种类 == 节_模块) continue;
            加子(根, c);
        }
    }
}

/* ---- 入口 ---- */
/* 编译并执行一个 .jy 文件，输出走当前的 stdout / stderr。
 * 拆出来是因为 JNI 那边也要用：同一份代码，桌面命令行和手机上跑。 */
int 解_跑文件(const char *路径)
{
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

    解_装引入(根, 路径);
    if (解_错) return 1;

    if (!生成(根)) {
        打印诊断();
        return 1;
    }

    解_建型录(根);

    中间函数 *主 = 解_找函数(&模块体, "主", 3);
    if (!主) { fprintf(stderr, "没有 主 函数\n"); return 1; }

    解值 返; 解_清值(&返);
    解_执行(主, NULL, 0, &返);
    fflush(stdout);
    return 解_错 ? 1 : 0;
}

#ifndef 手机_内嵌
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法：简解释 程序.jy\n");
        return 2;
    }
    return 解_跑文件(argv[1]);
}
#endif
