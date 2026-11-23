/* ============================================================================
 *  简出 —— 自己的汇编器 + 链接器
 *  ----------------------------------------------------------------------------
 *  原生后端 v3 的第一块地基。输入是 简汇编.c 吐出的那份 .s（一个刻意收窄的
 *  x86-64 子集），输出是一个能直接执行的 ELF64，全程不碰 as、不碰 ld、不碰 gcc。
 *
 *  为什么从"汇编文本"入手，而不是让后端直接吐机器码：
 *
 *    1. 文本仍然可读，出错了能对照 as 的结果一条条核。
 *    2. 后端那 1700 行的寄存器分配与指令选择一行都不用改，风险为零。
 *    3. 汇编器可以单独测：同一份 .s，as+ld 跑出来的输出和这里跑出来的输出
 *       逐字相同，才算对。
 *
 *  两个段：
 *    - 代码 + 只读数据（.text / .rodata）合成一个 R+X 的 PT_LOAD；
 *    - .bss 单独一个 R+W 的 PT_LOAD（filesz = 0，靠 memsz 撑开）。
 *
 *  编码一律走最宽的那一档：内存操作数统一 mod=10 + disp32，跳转一律 rel32。
 *  代价是每条指令多几个字节，换来的是「长度与位移无关」——一遍就能定长，
 *  不必反复迭代。这份汇编由我们自己生成，不追求榨干每一个字节。
 * ========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <sys/stat.h>

/* ---- 段 ---- */
enum { 段_代码 = 0, 段_只读 = 1, 段_bss = 2, 段数 = 3 };

static unsigned char *段文[段数];
static int 段长[段数], 段容[段数];
static int 当前段 = 段_代码;

static int 错数 = 0;

static void 坏(const char *因, const char *细节)
{
    if (错数 > 20) return;
    错数++;
    fprintf(stderr, "简出：%s", 因);
    if (细节 && *细节) fprintf(stderr, "（%s）", 细节);
    fputc('\n', stderr);
}

static void 备容(int 段, int 需)
{
    if (段长[段] + 需 <= 段容[段]) return;
    int 新 = 段容[段] ? 段容[段] : 4096;
    while (新 < 段长[段] + 需) 新 *= 2;
    段文[段] = (unsigned char *)realloc(段文[段], (size_t)新);
    if (!段文[段]) { fprintf(stderr, "简出：内存不够\n"); exit(1); }
    段容[段] = 新;
}

static void 发(unsigned char b)
{
    备容(当前段, 1);
    段文[当前段][段长[当前段]++] = b;
}

static void 发4(long long v)
{
    uint32_t u = (uint32_t)v;
    备容(当前段, 4);
    段文[当前段][段长[当前段]++] = (unsigned char)(u & 0xff);
    段文[当前段][段长[当前段]++] = (unsigned char)((u >> 8) & 0xff);
    段文[当前段][段长[当前段]++] = (unsigned char)((u >> 16) & 0xff);
    段文[当前段][段长[当前段]++] = (unsigned char)((u >> 24) & 0xff);
}

static void 发8(long long v)
{
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; i++) 发((unsigned char)((u >> (8 * i)) & 0xff));
}

/* ---- 待填的 rel32 ---- */
typedef struct {
    int 源段, 位置;      /* disp32 在段内的位置 */
    char 标[160];
} 待填项;

static 待填项 *待填表 = NULL;
static int 待数 = 0, 待容 = 0;

static void 记待填(const char *标)
{
    if (待数 >= 待容) {
        待容 = 待容 ? 待容 * 2 : 64;
        待填表 = (待填项 *)realloc(待填表, (size_t)待容 * sizeof(待填项));
        if (!待填表) { fprintf(stderr, "简出：内存不够\n"); exit(1); }
    }
    待填表[待数].源段 = 当前段;
    待填表[待数].位置 = 段长[当前段] - 4;   /* 刚发完四个字节 */
    snprintf(待填表[待数].标, sizeof(待填表[待数].标), "%s", 标);
    待数++;
}

/* ---- 标签 ---- */
typedef struct {
    char 名[160];
    int 段, 位置;
} 标签项;

static 标签项 *标签表 = NULL;
static int 标数 = 0, 标容 = 0;

static void 记标签(const char *名)
{
    for (int i = 0; i < 标数; i++) {
        if (strcmp(标签表[i].名, 名) == 0) {
            坏("标签重名", 名);
            return;
        }
    }
    if (标数 >= 标容) {
        标容 = 标容 ? 标容 * 2 : 64;
        标签表 = (标签项 *)realloc(标签表, (size_t)标容 * sizeof(标签项));
        if (!标签表) { fprintf(stderr, "简出：内存不够\n"); exit(1); }
    }
    snprintf(标签表[标数].名, sizeof(标签表[标数].名), "%s", 名);
    标签表[标数].段 = 当前段;
    标签表[标数].位置 = 段长[当前段];
    标数++;
}

static int 找标签(const char *名, int *段, int *位置)
{
    for (int i = 0; i < 标数; i++) {
        if (strcmp(标签表[i].名, 名) == 0) { *段 = 标签表[i].段; *位置 = 标签表[i].位置; return 1; }
    }
    return 0;
}

/* ============================================================================
 *  寄存器
 * ========================================================================== */

static const char *寄名64[16] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
};
static const char *寄名32[16] = {
    "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
    "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"
};
static const char *寄名8[16] = {
    "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil",
    "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"
};

/* 返回寄存器编号；-1 不认识。宽=64/32/8。xmm 用 16+n 表示。 */
static int 找寄(const char *名, int 宽)
{
    const char **表 = 宽 == 64 ? 寄名64 : 宽 == 32 ? 寄名32 : 寄名8;
    for (int i = 0; i < 16; i++)
        if (strcmp(表[i], 名) == 0) return i;
    if (名[0] == 'x' && 名[1] == 'm' && 名[2] == 'm') {
        char *尾 = NULL;
        long n = strtol(名 + 3, &尾, 10);
        if (尾 && !*尾 && n >= 0 && n < 16) return 16 + (int)n;
    }
    return -1;
}



/* ============================================================================
 *  操作数
 * ========================================================================== */

enum { 类_寄 = 0, 类_立即 = 1, 类_内存 = 2, 类_标签 = 3 };

typedef struct {
    int 类;
    int 寄;            /* 类_寄：寄存器号 */
    long long 值;      /* 类_立即：值；类_内存：disp */
    int 基, 索引, 倍;  /* 类_内存；-1 表示没有 */
    int 是rip;
    char 标[160];
} 操作;

static int 是xmm(操作 *o) { return o->类 == 类_寄 && o->寄 >= 16; }

static void 去空白(char **起)
{
    while (**起 && isspace((unsigned char)**起)) (*起)++;
    char *e = *起 + strlen(*起);
    while (e > *起 && isspace((unsigned char)e[-1])) *--e = 0;
}

static int 解操作(char *s, 操作 *o)
{
    去空白(&s);
    memset(o, 0, sizeof(*o));
    o->基 = o->索引 = -1;
    o->倍 = 1;
    if (!*s) return 0;

    if (s[0] == '$') {
        o->类 = 类_立即;
        /* 走无符号解析：除法的魔数常常大于 2^63，用 strtoll 会被截成
           LLONG_MAX —— 12 条 movabsq 全变成 0x7fff...，只在负数除法上错。 */
        o->值 = (long long)strtoull(s + 1, NULL, 0);
        return 1;
    }
    if (s[0] == '%') {
        int r = 找寄(s + 1, 64);
        if (r < 0) r = 找寄(s + 1, 32);
        if (r < 0) r = 找寄(s + 1, 8);
        if (r < 0) { 坏("不认识的寄存器", s); return 0; }
        o->类 = 类_寄;
        o->寄 = r;
        return 1;
    }

    char *括 = strchr(s, '(');
    if (!括) {
        /* 裸标签 */
        o->类 = 类_标签;
        snprintf(o->标, sizeof(o->标), "%s", s);
        return 1;
    }

    *括 = 0;
    o->类 = 类_内存;
    o->值 = *s ? strtoll(s, NULL, 0) : 0;

    char *内 = 括 + 1;
    char *右 = strchr(内, ')');
    if (右) *右 = 0;

    if (strcmp(内, "%rip") == 0) {
        o->是rip = 1;
        snprintf(o->标, sizeof(o->标), "%s", s);
        return 1;
    }

    /* %基, %索引, 倍 */
    char *段[3] = { 内, NULL, NULL };
    int 段数 = 1;
    for (char *p = 内; *p && 段数 < 3; p++) {
        if (*p == ',') { *p = 0; 段[段数++] = p + 1; }
    }
    if (段[0] && *段[0]) {
        去空白(&段[0]);
        if (段[0][0] != '%') { 坏("括号里应该是寄存器", 段[0]); return 0; }
        o->基 = 找寄(段[0] + 1, 64);
        if (o->基 < 0) { 坏("不认识的基址寄存器", 段[0]); return 0; }
    }
    if (段数 > 1 && 段[1] && *段[1]) {
        去空白(&段[1]);
        if (段[1][0] != '%') { 坏("索引应该是寄存器", 段[1]); return 0; }
        o->索引 = 找寄(段[1] + 1, 64);
        if (o->索引 < 0) { 坏("不认识的索引寄存器", 段[1]); return 0; }
    }
    if (段数 > 2 && 段[2]) {
        去空白(&段[2]);
        o->倍 = atoi(段[2]);
        if (o->倍 != 1 && o->倍 != 2 && o->倍 != 4 && o->倍 != 8) {
            坏("比例只能是 1/2/4/8", 段[2]);
            o->倍 = 1;
        }
    }
    return 1;
}

/* ============================================================================
 *  编码小工具
 * ========================================================================== */

/* REX：w=64 位，r/x/b 分别对应 modrm.reg、SIB.index、modrm.rm/基址的扩展位 */
static void 出rex(int w, int r, int x, int b)
{
    unsigned char v = (unsigned char)(0x40 | (w ? 8 : 0) | (r ? 4 : 0) | (x ? 2 : 0) | (b ? 1 : 0));
    if (v != 0x40) 发(v);
}

static int rm扩(操作 *o)
{
    if (o->类 == 类_寄) return (o->寄 & 15) >> 3;
    if (o->类 == 类_内存) return o->基 >= 0 ? (o->基 >> 3) : 0;
    return 0;
}

static int 索扩(操作 *o)
{
    if (o->类 == 类_内存 && o->索引 >= 0) return o->索引 >> 3;
    return 0;
}

/* 输出 r/m 字节串。regf 是 modrm 的 reg 字段（0..15）。 */
static void 编rm(int regf, 操作 *rm)
{
    if (rm->类 == 类_寄) {
        发((unsigned char)(0xC0 | ((regf & 7) << 3) | (rm->寄 & 7)));
        return;
    }
    if (rm->类 == 类_标签) {
        坏("标签不能直接当内存操作数", rm->标);
        return;
    }
    if (rm->类 != 类_内存) {
        坏("这里要的是寄存器或内存", "");
        return;
    }

    if (rm->是rip) {
        发((unsigned char)(0x00 | ((regf & 7) << 3) | 5));   /* mod=00 rm=101 */
        发4(0);
        记待填(rm->标);
        return;
    }
    if (rm->基 < 0) {
        /* 绝对地址 disp32：[disp] */
        发((unsigned char)(0x00 | ((regf & 7) << 3) | 4));
        发(0x25);                    /* SIB：index=100(无), base=101(无) */
        发4(rm->值);
        return;
    }

    int 基 = rm->基 & 7;
    if (rm->索引 < 0 && 基 != 4) {
        发((unsigned char)(0x80 | ((regf & 7) << 3) | 基));   /* mod=10 */
        发4(rm->值);
        return;
    }

    int 索 = rm->索引 < 0 ? 4 : (rm->索引 & 7);
    int 倍 = rm->索引 < 0 ? 0 : (rm->倍 == 8 ? 3 : rm->倍 == 4 ? 2 : rm->倍 == 2 ? 1 : 0);
    发((unsigned char)(0x80 | ((regf & 7) << 3) | 4));
    发((unsigned char)((倍 << 6) | (索 << 3) | 基));
    发4(rm->值);
}

/* SSE 指令：legacy 前缀 → REX → 0F 码 → modrm。w=1 表示要 REX.W。 */
static void sse(int 前缀, int 码, int regf, 操作 *rm, int w)
{
    if (前缀) 发((unsigned char)前缀);
    出rex(w, (regf & 15) >> 3, 索扩(rm), rm扩(rm));
    发(0x0F);
    发((unsigned char)码);
    编rm(regf & 15, rm);
}

/* 算术指令族的编号：对应 81/83 的 /digit */
static int 算术号(const char *名)
{
    if (!strcmp(名, "add")) return 0;
    if (!strcmp(名, "or"))  return 1;
    if (!strcmp(名, "and")) return 4;
    if (!strcmp(名, "sub")) return 5;
    if (!strcmp(名, "xor")) return 6;
    if (!strcmp(名, "cmp")) return 7;
    return -1;
}

/* 01 /r 这一族（r64 → r/m64）与 03 /r 这一族（r/m64 → r64） */
static int 算术OP1(const char *名)
{
    if (!strcmp(名, "add")) return 0x01;
    if (!strcmp(名, "or"))  return 0x09;
    if (!strcmp(名, "and")) return 0x21;
    if (!strcmp(名, "sub")) return 0x29;
    if (!strcmp(名, "xor")) return 0x31;
    if (!strcmp(名, "cmp")) return 0x39;
    return -1;
}
static int 算术OP2(const char *名)
{
    if (!strcmp(名, "add")) return 0x03;
    if (!strcmp(名, "or"))  return 0x0B;
    if (!strcmp(名, "and")) return 0x23;
    if (!strcmp(名, "sub")) return 0x2B;
    if (!strcmp(名, "xor")) return 0x33;
    if (!strcmp(名, "cmp")) return 0x3B;
    return -1;
}

/* jcc 的条件码高字节（0F 8x） */
static int 条件码(const char *名)
{
    if (!strcmp(名, "jo"))   return 0x80;
    if (!strcmp(名, "jno"))  return 0x81;
    if (!strcmp(名, "jb") || !strcmp(名, "jc") || !strcmp(名, "jnae")) return 0x82;
    if (!strcmp(名, "jae") || !strcmp(名, "jnb") || !strcmp(名, "jnc")) return 0x83;
    if (!strcmp(名, "je") || !strcmp(名, "jz"))  return 0x84;
    if (!strcmp(名, "jne") || !strcmp(名, "jnz")) return 0x85;
    if (!strcmp(名, "jbe") || !strcmp(名, "jna")) return 0x86;
    if (!strcmp(名, "ja") || !strcmp(名, "jnbe")) return 0x87;
    if (!strcmp(名, "js"))   return 0x88;
    if (!strcmp(名, "jns"))  return 0x89;
    if (!strcmp(名, "jp") || !strcmp(名, "jpe")) return 0x8A;
    if (!strcmp(名, "jnp") || !strcmp(名, "jpo")) return 0x8B;
    if (!strcmp(名, "jl") || !strcmp(名, "jnge")) return 0x8C;
    if (!strcmp(名, "jge") || !strcmp(名, "jnl")) return 0x8D;
    if (!strcmp(名, "jle") || !strcmp(名, "jng")) return 0x8E;
    if (!strcmp(名, "jg") || !strcmp(名, "jnle")) return 0x8F;
    return -1;
}

/* setcc 的低字节（0F 9x） */
static int 置条件码(const char *名)
{
    if (!strcmp(名, "seto"))  return 0x90;
    if (!strcmp(名, "setno")) return 0x91;
    if (!strcmp(名, "setb") || !strcmp(名, "setc")) return 0x92;
    if (!strcmp(名, "setae") || !strcmp(名, "setnc")) return 0x93;
    if (!strcmp(名, "sete") || !strcmp(名, "setz"))  return 0x94;
    if (!strcmp(名, "setne") || !strcmp(名, "setnz")) return 0x95;
    if (!strcmp(名, "setbe")) return 0x96;
    if (!strcmp(名, "seta"))  return 0x97;
    if (!strcmp(名, "sets"))  return 0x98;
    if (!strcmp(名, "setns")) return 0x99;
    if (!strcmp(名, "setl"))  return 0x9C;
    if (!strcmp(名, "setge")) return 0x9D;
    if (!strcmp(名, "setle")) return 0x9E;
    if (!strcmp(名, "setg"))  return 0x9F;
    return -1;
}

/* ============================================================================
 *  指令编码
 *  ----------------------------------------------------------------------------
 *  入口：名 = 助记符（已去后缀？不，带着后缀，如 movq），ops = 操作数数组
 * ========================================================================== */

static void 编指令(const char *名, 操作 *o, int 数)
{
    char 基[32];
    int 宽 = 0;                 /* 64 / 32 / 8，按后缀 */
    size_t 长 = strlen(名);

    /* 这几个助记符本身就以 q/l/b/w 结尾，不能当后缀剥。
       call 少了这条就会被剥成 "cal" —— 一个只有 4 个字母的坑。 */
    static const char *无后缀[] = {
        "call", "jmp", "ret", "leave", "syscall", "cqto", "nop", "rep", NULL
    };
    int 裸 = 0;
    for (int i = 0; 无后缀[i]; i++)
        if (!strcmp(名, 无后缀[i])) { 裸 = 1; break; }

    if (!裸 && 长 && (名[长 - 1] == 'q' || 名[长 - 1] == 'l' ||
                      名[长 - 1] == 'b' || 名[长 - 1] == 'w')) {
        宽 = 名[长 - 1] == 'q' ? 64 : 名[长 - 1] == 'l' ? 32 : 名[长 - 1] == 'b' ? 8 : 16;
        snprintf(基, sizeof(基), "%.*s", (int)(长 - 1), 名);
    } else {
        snprintf(基, sizeof(基), "%s", 名);
    }

    int 是64 = (宽 == 64);

    /* ---------- 无操作数 ---------- */
    if (!strcmp(基, "ret") && 长 == 3) { 发(0xC3); return; }
    if (!strcmp(基, "leave") && 长 == 5) { 发(0xC9); return; }
    if (!strcmp(基, "syscall")) { 发(0x0F); 发(0x05); return; }
    if (!strcmp(基, "cqto")) { 发(0x48); 发(0x99); return; }
    if (!strcmp(基, "nop")) { 发(0x90); return; }
    if (!strcmp(基, "rep") && 数 >= 1 && !strcmp(o[0].标, "movsb")) {
        发(0xF3); 发(0xA4); return;
    }

    /* ---------- mov ---------- */
    if (!strcmp(基, "mov") || !strcmp(基, "movabs")) {
        if (数 != 2) { 坏("mov 要两个操作数", 名); return; }

        /* movq 在 xmm 与通用寄存器之间传 64 位：66 REX.W 0F 6E/7E */
        if (是xmm(&o[0]) || 是xmm(&o[1])) {
            if (是xmm(&o[1])) sse(0x66, 0x6E, o[1].寄, &o[0], 1);
            else sse(0x66, 0x7E, o[0].寄, &o[1], 1);
            return;
        }

        if (o[0].类 == 类_立即 && (o[1].类 == 类_寄 || o[1].类 == 类_内存)) {
            long long v = o[0].值;
            if (宽 == 32 && o[1].类 == 类_寄) {
                /* B8+rd id */
                if (o[1].寄 >= 8) 出rex(0, 0, 0, 1);
                发((unsigned char)(0xB8 + (o[1].寄 & 7)));
                发4(v);
                return;
            }
            if (宽 == 8) {
                if (o[1].类 == 类_内存) {
                    出rex(0, 0, 索扩(&o[1]), rm扩(&o[1]));
                    发(0xC6); 编rm(0, &o[1]);
                    发((unsigned char)(v & 0xff));
                } else {
                    /* movb $imm, %reg8 */
                    if (o[1].寄 >= 4) 出rex(0, 0, 0, rm扩(&o[1]));
                    发((unsigned char)(0xB0 + (o[1].寄 & 7)));
                    发((unsigned char)(v & 0xff));
                }
                return;
            }
            /* 64 位 */
            if (!strcmp(基, "movabs") || v > 0x7fffffffLL || v < -2147483648LL) {
                出rex(1, 0, 0, rm扩(&o[1]));
                发((unsigned char)(0xB8 + (o[1].寄 & 7)));
                发8(v);
                return;
            }
            /* C7 /0 id */
            出rex(1, 0, 索扩(&o[1]), rm扩(&o[1]));
            发(0xC7);
            编rm(0, &o[1]);
            发4(v);
            return;
        }

        if (o[0].类 == 类_寄 && o[1].类 == 类_寄) {
            if (宽 == 8) {
                出rex(0, o[0].寄 >> 3, 0, o[1].寄 >> 3);
                发(0x88);
            } else {
                出rex(是64, o[0].寄 >> 3, 0, o[1].寄 >> 3);
                发(0x89);
            }
            编rm(o[0].寄, &o[1]);
            return;
        }
        if (o[0].类 == 类_寄 && o[1].类 == 类_内存) {
            if (宽 == 8) {
                出rex(0, o[0].寄 >> 3, 索扩(&o[1]), rm扩(&o[1]));
                发(0x88);
            } else {
                出rex(是64, o[0].寄 >> 3, 索扩(&o[1]), rm扩(&o[1]));
                发(0x89);
            }
            编rm(o[0].寄, &o[1]);
            return;
        }
        if (o[0].类 == 类_内存 && o[1].类 == 类_寄) {
            if (宽 == 8) {
                出rex(0, o[1].寄 >> 3, 索扩(&o[0]), rm扩(&o[0]));
                发(0x8A);
            } else {
                出rex(是64, o[1].寄 >> 3, 索扩(&o[0]), rm扩(&o[0]));
                发(0x8B);
            }
            编rm(o[1].寄, &o[0]);
            return;
        }
        if (o[0].类 == 类_标签 && o[1].类 == 类_寄) {
            /* movq 标签, %reg —— 当成绝对地址 48 C7 /0 id 放不下，用 B8 立即数 + 待填 */
            出rex(1, 0, 0, o[1].寄 >> 3);
            发((unsigned char)(0xB8 + (o[1].寄 & 7)));
            发4(0);
            记待填(o[0].标);
            return;
        }
        坏("这种 mov 组合", 名);
        return;
    }

    /* ---------- lea ---------- */
    if (!strcmp(基, "lea")) {
        if (数 != 2 || o[0].类 != 类_内存 || o[1].类 != 类_寄) {
            坏("lea 要 内存 → 寄存器", 名);
            return;
        }
        出rex(是64, o[1].寄 >> 3, 索扩(&o[0]), rm扩(&o[0]));
        发(0x8D);
        编rm(o[1].寄, &o[0]);
        return;
    }

    /* ---------- 算术族：add/sub/and/or/xor/cmp ---------- */
    {
        int 号 = 算术号(基);
        if (号 >= 0) {
            if (数 != 2) { 坏("算术指令要两个操作数", 名); return; }
            if (o[0].类 == 类_立即) {
                long long v = o[0].值;
                int 小 = (v >= -128 && v <= 127);
                if (宽 == 8) {
                    出rex(0, 0, 索扩(&o[1]), rm扩(&o[1]));
                    发(0x80); 编rm(号, &o[1]);
                    发((unsigned char)(v & 0xff));
                    return;
                }
                出rex(是64, 0, 索扩(&o[1]), rm扩(&o[1]));
                发(小 ? 0x83 : 0x81);
                编rm(号, &o[1]);
                if (小) 发((unsigned char)(v & 0xff));
                else 发4(v);
                return;
            }
            if (o[0].类 == 类_寄) {
                /* 01 /r：r64 → r/m64 */
                出rex(是64, o[0].寄 >> 3, 索扩(&o[1]), rm扩(&o[1]));
                发((unsigned char)算术OP1(基));
                编rm(o[0].寄, &o[1]);
                return;
            }
            if (o[0].类 == 类_内存 && o[1].类 == 类_寄) {
                /* 03 /r：r/m64 → r64 */
                出rex(是64, o[1].寄 >> 3, 索扩(&o[0]), rm扩(&o[0]));
                发((unsigned char)算术OP2(基));
                编rm(o[1].寄, &o[0]);
                return;
            }
            坏("这种算术组合", 名);
            return;
        }
    }

    /* ---------- test ---------- */
    if (!strcmp(基, "test")) {
        if (数 != 2) { 坏("test 要两个操作数", 名); return; }
        if (o[0].类 == 类_立即) {
            出rex(是64, 0, 索扩(&o[1]), rm扩(&o[1]));
            发(0xF7); 编rm(0, &o[1]);
            发4(o[0].值);
            return;
        }
        if (o[0].类 == 类_寄) {
            出rex(是64, o[0].寄 >> 3, 索扩(&o[1]), rm扩(&o[1]));
            发(0x85);
            编rm(o[0].寄, &o[1]);
            return;
        }
        坏("这种 test 组合", 名);
        return;
    }

    /* ---------- imul ---------- */
    if (!strcmp(基, "imul")) {
        if (数 == 2 && o[0].类 == 类_寄 && o[1].类 == 类_寄) {
            /* 0F AF /r：reg = reg * rm */
            出rex(是64, o[1].寄 >> 3, 0, o[0].寄 >> 3);
            发(0x0F); 发(0xAF);
            编rm(o[1].寄, &o[0]);
            return;
        }
        if (数 == 2 && o[0].类 == 类_立即 && o[1].类 == 类_寄) {
            /* 69 /r id：reg = reg * imm */
            出rex(是64, o[1].寄 >> 3, 0, o[1].寄 >> 3);
            发(0x69);
            编rm(o[1].寄, &o[1]);
            发4(o[0].值);
            return;
        }
        if (数 == 3 && o[0].类 == 类_立即 && o[2].类 == 类_寄) {
            /* 69 /r id：reg = rm * imm。三操作数形式，源和目标可以不同。
             * 后端的「乘常量」就靠它：imulq $31, %r9, %rsi —— 一步出结果，
             * 不必先 mov 再乘，也不会踩掉源寄存器。 */
            if (o[0].值 != (long long)(int)o[0].值) {
                坏("imul 的立即数超出 32 位，得先装进寄存器", 名);
                return;
            }
            出rex(是64, o[2].寄 >> 3, 0, o[1].类 == 类_寄 ? o[1].寄 >> 3 : 0);
            发(0x69);
            编rm(o[2].寄, &o[1]);
            发4(o[0].值);
            return;
        }
        坏("这种 imul 组合", 名);
        return;
    }

    /* ---------- 乘除 ---------- */
    if (!strcmp(基, "mul") || !strcmp(基, "div") || !strcmp(基, "idiv")) {
        if (数 != 1) { 坏("这只收一个操作数", 名); return; }
        int 号 = !strcmp(基, "mul") ? 4 : !strcmp(基, "div") ? 6 : 7;
        出rex(是64, 0, 0, rm扩(&o[0]));
        发(0xF7);
        编rm(号, &o[0]);
        return;
    }

    /* ---------- neg / inc / dec ---------- */
    if (!strcmp(基, "neg")) {
        if (数 != 1) { 坏("neg 收一个操作数", 名); return; }
        出rex(是64, 0, 0, rm扩(&o[0]));
        发(0xF7); 编rm(3, &o[0]);
        return;
    }
    if (!strcmp(基, "inc") || !strcmp(基, "dec")) {
        if (数 != 1) { 坏("inc/dec 收一个操作数", 名); return; }
        出rex(是64, 0, 0, rm扩(&o[0]));
        发(0xFF); 编rm(!strcmp(基, "inc") ? 0 : 1, &o[0]);
        return;
    }

    /* ---------- 移位 ---------- */
    if (!strcmp(基, "shl") || !strcmp(基, "shr") || !strcmp(基, "sar")) {
        if (数 != 2) { 坏("移位要两个操作数", 名); return; }
        int 号 = !strcmp(基, "shl") ? 4 : !strcmp(基, "shr") ? 5 : 7;
        if (o[0].类 == 类_立即) {
            出rex(是64, 0, 索扩(&o[1]), rm扩(&o[1]));
            发(0xC1); 编rm(号, &o[1]);
            发((unsigned char)(o[0].值 & 0xff));
            return;
        }
        if (o[0].类 == 类_寄 && o[0].寄 == 1) {
            出rex(是64, 0, 索扩(&o[1]), rm扩(&o[1]));
            发(0xD3); 编rm(号, &o[1]);
            return;
        }
        坏("这种移位组合", 名);
        return;
    }

    /* ---------- 位测试：bt/bts/btr/btc $imm, r/m ---------- */
    if (!strcmp(基, "bt") || !strcmp(基, "bts") || !strcmp(基, "btr") || !strcmp(基, "btc")) {
        if (数 != 2 || o[0].类 != 类_立即) { 坏("位测试要 立即数, 寄存器", 名); return; }
        int 号 = 基[2] == 's' ? 5 : 基[2] == 'r' ? 6 : 基[2] == 'c' ? 7 : 4;
        出rex(是64, 0, 索扩(&o[1]), rm扩(&o[1]));
        发(0x0F); 发(0xBA);
        编rm(号, &o[1]);
        发((unsigned char)(o[0].值 & 0xff));
        return;
    }

    /* ---------- movslq / movzbq ---------- */
    if (!strcmp(基, "movsl")) {           /* movslq */
        if (数 != 2 || o[1].类 != 类_寄) { 坏("movslq 要 源 → 寄存器", 名); return; }
        出rex(1, o[1].寄 >> 3, 索扩(&o[0]), rm扩(&o[0]));
        发(0x63);
        编rm(o[1].寄, &o[0]);
        return;
    }
    if (!strcmp(基, "movzb")) {           /* movzbq */
        if (数 != 2 || o[1].类 != 类_寄) { 坏("movzbq 要 源 → 寄存器", 名); return; }
        出rex(1, o[1].寄 >> 3, 索扩(&o[0]), rm扩(&o[0]));
        发(0x0F); 发(0xB6);
        编rm(o[1].寄, &o[0]);
        return;
    }

    /* ---------- push / pop ---------- */
    if (!strcmp(基, "push") || !strcmp(基, "pop")) {
        int 推 = !strcmp(基, "push");
        if (数 != 1) { 坏("push/pop 收一个操作数", 名); return; }
        if (o[0].类 == 类_寄) {
            if (o[0].寄 >= 8) 出rex(0, 0, 0, 1);
            发((unsigned char)((推 ? 0x50 : 0x58) + (o[0].寄 & 7)));
            return;
        }
        if (推 && o[0].类 == 类_立即) {
            long long v = o[0].值;
            if (v >= -128 && v <= 127) { 发(0x6A); 发((unsigned char)(v & 0xff)); }
            else { 发(0x68); 发4(v); }
            return;
        }
        坏("这种 push/pop", 名);
        return;
    }

    /* ---------- 跳转 ---------- */
    if (!strcmp(基, "jmp")) {
        if (数 != 1) { 坏("jmp 收一个目标", 名); return; }
        if (o[0].类 == 类_寄) {
            出rex(0, 0, 0, o[0].寄 >> 3);
            发(0xFF); 编rm(4, &o[0]);
            return;
        }
        发(0xE9); 发4(0);
        记待填(o[0].标);
        return;
    }
    {
        int cc = 条件码(名);
        if (cc > 0) {
            if (数 != 1) { 坏("条件跳转收一个目标", 名); return; }
            发(0x0F); 发((unsigned char)cc);
            发4(0);
            记待填(o[0].标);
            return;
        }
    }
    {
        int cc = 置条件码(名);
        if (cc > 0) {
            if (数 != 1 || o[0].类 != 类_寄) { 坏("setcc 收一个 8 位寄存器", 名); return; }
            if (o[0].寄 >= 4) 出rex(0, 0, 0, o[0].寄 >> 3);
            发(0x0F); 发((unsigned char)cc);
            编rm(0, &o[0]);
            return;
        }
    }

    /* ---------- call ---------- */
    if (!strcmp(基, "call")) {
        if (数 != 1) { 坏("call 收一个目标", 名); return; }
        if (o[0].类 == 类_寄) {
            出rex(0, 0, 0, o[0].寄 >> 3);
            发(0xFF); 编rm(2, &o[0]);
            return;
        }
        发(0xE8); 发4(0);
        记待填(o[0].标);
        return;
    }

    /* ---------- SSE 双精度 ---------- */
    if (!strcmp(名, "movsd")) {
        if (数 != 2) { 坏("movsd 要两个操作数", 名); return; }
        if (是xmm(&o[1])) sse(0xF2, 0x10, o[1].寄, &o[0], 0);
        else if (是xmm(&o[0])) sse(0xF2, 0x11, o[0].寄, &o[1], 0);
        else 坏("movsd 至少要有一个 xmm", 名);
        return;
    }
    {
        int 码 = 0;
        if (!strcmp(名, "addsd")) 码 = 0x58;
        else if (!strcmp(名, "subsd")) 码 = 0x5C;
        else if (!strcmp(名, "mulsd")) 码 = 0x59;
        else if (!strcmp(名, "divsd")) 码 = 0x5E;
        else if (!strcmp(名, "sqrtsd")) 码 = 0x51;
        else if (!strcmp(名, "minsd")) 码 = 0x5D;
        else if (!strcmp(名, "maxsd")) 码 = 0x5F;
        if (码) {
            if (数 != 2 || !是xmm(&o[1])) { 坏("这个 SSE 指令要 xmm 作目标", 名); return; }
            sse(0xF2, 码, o[1].寄, &o[0], 0);
            return;
        }
    }
    if (!strcmp(名, "ucomisd") || !strcmp(名, "comisd")) {
        if (数 != 2 || !是xmm(&o[1])) { 坏("比较要 xmm 作第二个操作数", 名); return; }
        sse(0x66, 名[0] == 'u' ? 0x2E : 0x2F, o[1].寄, &o[0], 0);
        return;
    }
    if (!strcmp(名, "cvtsi2sd")) {
        if (数 != 2 || !是xmm(&o[1])) { 坏("cvtsi2sd 要 寄存器 → xmm", 名); return; }
        sse(0xF2, 0x2A, o[1].寄, &o[0], 1);
        return;
    }
    if (!strcmp(名, "cvttsd2si") || !strcmp(名, "cvtsd2si")) {
        /* 源可以是 xmm，也可以是内存里的 double；目标只能是通用寄存器 */
        if (数 != 2 || 是xmm(&o[1]) || o[1].类 != 类_寄) { 坏("这条要 xmm/内存 → 通用寄存器", 名); return; }
        /* cvttsd2si 的第 4 个字母是 t（截断），cvtsd2si 是 s（按当前舍入） */
        sse(0xF2, 名[3] == 't' ? 0x2C : 0x2D, o[1].寄, &o[0], 1);
        return;
    }
    if (!strcmp(名, "xorpd") || !strcmp(名, "pxor")) {
        if (数 != 2 || !是xmm(&o[1])) { 坏("xorpd 要 xmm 作目标", 名); return; }
        sse(0x66, 0x57, o[1].寄, &o[0], 0);
        return;
    }

    坏("不认识的助记符", 名);
}

/* ============================================================================
 *  伪指令
 * ========================================================================== */

static void 填到对齐(int n)
{
    if (n <= 1) return;
    while (段长[当前段] % n) 发(当前段 == 段_代码 ? 0x90 : 0x00);
}

static void 编伪指令(char *行)
{
    char *p = 行;

    if (!strcmp(p, ".text")) { 当前段 = 段_代码; return; }
    if (!strcmp(p, ".bss")) { 当前段 = 段_bss; return; }
    if (!strcmp(p, ".data")) { 当前段 = 段_只读; return; }
    if (!strncmp(p, ".section", 8)) {
        char *名 = p + 8;
        去空白(&名);
        if (strstr(名, "rodata") || strstr(名, "data")) { 当前段 = 段_只读; return; }
        if (strstr(名, "text")) { 当前段 = 段_代码; return; }
        if (strstr(名, "bss")) { 当前段 = 段_bss; return; }
        当前段 = 段_只读;
        return;
    }
    if (!strncmp(p, ".globl", 6) || !strncmp(p, ".global", 7)) return;
    if (!strncmp(p, ".type", 5) || !strncmp(p, ".size", 5) || !strncmp(p, ".ident", 6)) return;

    if (!strncmp(p, ".align", 6) || !strncmp(p, ".balign", 7)) {
        char *q = strchr(p, ' ');
        填到对齐(q ? atoi(q + 1) : 1);
        return;
    }
    if (!strncmp(p, ".skip", 5) || !strncmp(p, ".space", 6)) {
        char *q = strchr(p, ' ');
        int n = q ? atoi(q + 1) : 0;
        for (int i = 0; i < n; i++) 发(0);
        return;
    }
    if (!strncmp(p, ".byte", 5)) {
        char *q = p + 5;
        while (*q) {
            while (*q && (*q == ',' || isspace((unsigned char)*q))) q++;
            if (!*q) break;
            long v = strtol(q, &q, 0);
            发((unsigned char)(v & 0xff));
        }
        return;
    }
    if (!strncmp(p, ".quad", 5)) {
        char *q = p + 5;
        while (*q) {
            while (*q && (*q == ',' || isspace((unsigned char)*q))) q++;
            if (!*q) break;
            long long v = (long long)strtoull(q, &q, 0);
            发8(v);
        }
        return;
    }
    if (!strncmp(p, ".asciz", 6) || !strncmp(p, ".string", 7)) {
        char *q = strchr(p, '"');
        if (!q) return;
        q++;
        while (*q && *q != '"') {
            if (*q == '\\' && q[1]) {
                q++;
                switch (*q) {
                case 'n': 发('\n'); break;
                case 't': 发('\t'); break;
                case 'r': 发('\r'); break;
                case '0': 发(0); break;
                default: 发((unsigned char)*q); break;
                }
            } else 发((unsigned char)*q);
            q++;
        }
        发(0);
        return;
    }
    if (!strncmp(p, ".long", 5) || !strncmp(p, ".int", 4)) {
        char *q = strchr(p, ' ');
        if (q) 发4(strtoll(q + 1, NULL, 0));
        return;
    }

    坏("不认识的伪指令", p);
}

/* ============================================================================
 *  一行 → 汇编
 * ========================================================================== */

static void 编行(char *行)
{
    去空白(&行);
    if (!*行) return;

    /* 标签（可能和指令/伪指令同一行） */
    char *冒 = strchr(行, ':');
    if (冒) {
        /* 冒号后面必须是空白或结尾，避免把别的东西误判成标签 */
        if (冒[1] == 0 || isspace((unsigned char)冒[1])) {
            *冒 = 0;
            char *名 = 行;
            去空白(&名);
            if (*名) 记标签(名);
            行 = 冒 + 1;
            去空白(&行);
            if (!*行) return;
        }
    }

    if (*行 == '.') { 编伪指令(行); return; }

    /* 助记符 */
    char *空 = 行;
    while (*空 && !isspace((unsigned char)*空)) 空++;
    char 名[32];
    snprintf(名, sizeof(名), "%.*s", (int)(空 - 行), 行);
    char *余 = *空 ? 空 + 1 : 空;
    去空白(&余);

    操作 操[3];
    int 数 = 0;
    if (*余) {
        char *出[3];
        int n = 0;
        出[n++] = 余;
        int 深 = 0;
        for (char *q = 余; *q && n < 3; q++) {
            if (*q == '(') 深++;
            else if (*q == ')') 深--;
            else if (*q == ',' && 深 == 0) { *q = 0; 出[n++] = q + 1; }
        }
        for (int i = 0; i < n; i++)
            if (解操作(出[i], &操[数])) 数++;
    }

    编指令(名, 操, 数);
}

/* ============================================================================
 *  ELF 写出
 * ========================================================================== */

static void 写16(unsigned char *p, unsigned v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void 写32(unsigned char *p, uint32_t v)
{
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
static void 写64(unsigned char *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)((v >> (8 * i)) & 0xff);
}

#define 基址 0x400000ULL
#define 页 0x1000ULL

static uint64_t 对页(uint64_t v) { return (v + 页 - 1) & ~(页 - 1); }

static int 出文件(const char *路)
{
    int 头长 = 64 + 56 * 2;
    uint64_t 代码长 = (uint64_t)段长[段_代码];
    uint64_t 只读长 = (uint64_t)段长[段_只读];
    uint64_t 正文长 = 代码长 + 只读长;
    uint64_t 正文起 = 基址 + (uint64_t)头长;
    uint64_t 正文止 = 正文起 + 正文长;

    uint64_t bss起 = 对页(正文止);
    uint64_t bss长 = (uint64_t)段长[段_bss];
    uint64_t 文件止 = 对页((uint64_t)头长 + 正文长);

    unsigned char *头 = (unsigned char *)calloc(1, (size_t)文件止);
    if (!头) { fprintf(stderr, "简出：内存不够\n"); return 0; }

    /* e_ident */
    头[0] = 0x7f; 头[1] = 'E'; 头[2] = 'L'; 头[3] = 'F';
    头[4] = 2;      /* ELFCLASS64 */
    头[5] = 1;      /* ELFDATA2LSB */
    头[6] = 1;      /* EV_CURRENT */
    写16(头 + 16, 2);            /* ET_EXEC */
    写16(头 + 18, 62);           /* EM_X86_64 */
    写32(头 + 20, 1);
    /* e_entry 稍后填 */
    写64(头 + 32, 64);           /* e_phoff */
    写64(头 + 40, 0);            /* e_shoff */
    写32(头 + 48, 0);
    写16(头 + 52, 64);
    写16(头 + 54, 56);
    写16(头 + 56, 2);            /* phnum */
    写16(头 + 58, 0);
    写16(头 + 60, 0);
    写16(头 + 62, 0);

    /* 程序头 1：R+X */
    unsigned char *p = 头 + 64;
    写32(p + 0, 1);              /* PT_LOAD */
    写32(p + 4, 5);              /* R|X */
    写64(p + 8, 0);              /* offset */
    写64(p + 16, 基址);
    写64(p + 24, 基址);
    写64(p + 32, (uint64_t)头长 + 正文长);
    写64(p + 40, (uint64_t)头长 + 正文长);
    写64(p + 48, 页);

    /* 程序头 2：R+W（.bss） */
    p = 头 + 64 + 56;
    写32(p + 0, 1);
    写32(p + 4, 6);
    写64(p + 8, 文件止);
    写64(p + 16, bss起);
    写64(p + 24, bss起);
    写64(p + 32, 0);             /* filesz = 0 */
    写64(p + 40, bss长);
    写64(p + 48, 页);

    /* 正文 */
    if (代码长) memcpy(头 + 头长, 段文[段_代码], (size_t)代码长);
    if (只读长) memcpy(头 + 头长 + 代码长, 段文[段_只读], (size_t)只读长);

    /* 入口 */
    int 段, 位;
    if (!找标签("_start", &段, &位)) {
        fprintf(stderr, "简出：找不到 _start，程序没法开始\n");
        free(头);
        return 0;
    }
    uint64_t 入口 = (段 == 段_代码 ? 正文起 : 正文起 + 代码长) + (uint64_t)位;
    写64(头 + 24, 入口);

    FILE *f = fopen(路, "wb");
    if (!f) { fprintf(stderr, "简出：写不了 %s\n", 路); free(头); return 0; }
    if (文件止 && fwrite(头, 1, (size_t)文件止, f) != (size_t)文件止) {
        fprintf(stderr, "简出：写 %s 时出错\n", 路);
        fclose(f); free(头); return 0;
    }
    fclose(f);
    free(头);
    chmod(路, 0755);
    return 1;
}

/* ============================================================================
 *  回填
 * ========================================================================== */

static int 回填(uint64_t 代码起, uint64_t 只读起, uint64_t bss起)
{
    int 坏项 = 0;
    for (int i = 0; i < 待数; i++) {
        待填项 *t = &待填表[i];
        int 目段, 目位;
        if (!找标签(t->标, &目段, &目位)) {
            坏("找不到标签", t->标);
            坏项++;
            continue;
        }
        uint64_t 源基 = t->源段 == 段_代码 ? 代码起 : t->源段 == 段_只读 ? 只读起 : bss起;
        uint64_t 目基 = 目段 == 段_代码 ? 代码起 : 目段 == 段_只读 ? 只读起 : bss起;

        uint64_t 下条 = 源基 + (uint64_t)t->位置 + 4;
        uint64_t 目标 = 目基 + (uint64_t)目位;
        int64_t 差 = (int64_t)(目标 - 下条);
        if (差 > 0x7fffffffLL || 差 < -0x80000000LL) {
            坏("相对位移超出 2GB", t->标);
            坏项++;
            continue;
        }
        unsigned char *p = 段文[t->源段] + t->位置;
        写32(p, (uint32_t)(int32_t)差);
    }
    return 坏项 == 0;
}

/* ============================================================================
 *  主流程
 * ========================================================================== */

static char *读全(const char *路, int *出长)
{
    FILE *f = fopen(路, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = (char *)malloc((size_t)n + 1);
    if (!p) { fclose(f); return NULL; }
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    p[n] = 0;
    fclose(f);
    *出长 = (int)n;
    return p;
}

/* 剥掉块注释与行注释，就地改写 */
static void 剥注释(char *s)
{
    int 在块 = 0;
    for (char *p = s; *p; p++) {
        if (!在块 && p[0] == '/' && p[1] == '*') { 在块 = 1; p[0] = ' '; p[1] = ' '; p++; continue; }
        if (在块 && p[0] == '*' && p[1] == '/') { 在块 = 0; p[0] = ' '; p[1] = ' '; p++; continue; }
        if (在块) { if (*p != '\n') *p = ' '; continue; }
        if (*p == '#') { while (*p && *p != '\n') *p++ = ' '; if (!*p) break; p--; }
    }
}

/* 每次开工前清零。简编译 内嵌调用时会连跑好几份，缓冲区留着复用。 */
static void 简出_重置(void)
{
    for (int i = 0; i < 段数; i++) 段长[i] = 0;
    当前段 = 段_代码;
    待数 = 0;
    标数 = 0;
    错数 = 0;
}

int 简出(const char *入路, const char *出路)
{
    简出_重置();

    int 长 = 0;
    char *文 = 读全(入路, &长);
    if (!文) { fprintf(stderr, "简出：读不了 %s\n", 入路); return 1; }

    剥注释(文);

    /* 逐行 */
    char *行 = 文;
    while (*行) {
        char *尾 = strchr(行, '\n');
        if (尾) *尾 = 0;
        编行(行);
        if (!尾) break;
        行 = 尾 + 1;
    }
    free(文);

    if (错数) { fprintf(stderr, "简出：%d 处错误，没有输出\n", 错数); return 1; }

    /* 段基址 */
    uint64_t 代码起 = 基址 + 176;
    uint64_t 只读起 = 代码起 + (uint64_t)段长[段_代码];
    uint64_t bss起 = 对页(只读起 + (uint64_t)段长[段_只读]);

    回填(代码起, 只读起, bss起);

    if (错数) { fprintf(stderr, "简出：%d 处错误，没有输出\n", 错数); return 1; }

    if (!出文件(出路)) return 1;
    return 0;
}

#ifndef 简出_内嵌
int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "简出 —— 简言自己的汇编器与链接器（不经过 as / ld）\n\n"
                "  简出 输入.s 输出\n");
        return 1;
    }
    return 简出(argv[1], argv[2]);
}
#endif
