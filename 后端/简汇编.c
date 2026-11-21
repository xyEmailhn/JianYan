/* ============================================================================
 *  原生后端：IR → x86-64 汇编
 *  ----------------------------------------------------------------------------
 *  和 C 后端的区别就一句话：不再把 IR 翻成 C 交给 gcc，而是直接吐汇编，
 *  自己调 as 和 ld。产出里没有一行 C。
 *
 *  这一版故意做得很笨，笨到没有 bug 的空间：
 *
 *    每个 IR 值  → 一个固定的 16 字节栈槽，-16*(v+1)(%rbp)
 *    每个基本块  → 一个标签 .L<函数序>_<块号>
 *    每个临时值  → 先算进 %rax，再存回槽
 *
 *  不分配寄存器、不复用槽、不做活跃变量分析——gcc 在 C 后端里干的那些活，
 *  这一版全都不干。慢是必然的，但它能证明「简言不经过 C 也能变成机器码」。
 *  下一步才是把栈槽换成寄存器，那是另一件事。
 *
 *  为什么槽是 16 字节而不是 8：字符串在运行时是 (指针, 长度) 两个机器字。
 *  统一 16 字节，整数往低 8 字节一放就行，不用为不同类型写两套搬运代码。
 *
 *  运行时用裸 syscall（write / mmap / exit），不链接 libc。
 *  这样 ld 出来的是真正的静态单文件，`ldd` 会说 "not a dynamic executable"。
 * ========================================================================== */

#define 汇_最多串 2048
#define 汇_最多函 512

static int 汇_失败 = 0;

typedef struct { unsigned char *字节; int 长; } 汇串项;
static 汇串项 汇_串[汇_最多串];
static int 汇_串数 = 0;

static 中间函数 *汇_函[汇_最多函];
static int 汇_函数 = 0;

/* 值号 → 栈偏移。负数，因为栈往下长。 */
static int 槽(int v) { return -16 * (v + 1); }

static int 汇_序号(中间函数 *f)
{
    for (int i = 0; i < 汇_函数; i++) if (汇_函[i] == f) return i;
    return -1;
}

static void 汇_坏(中间函数 *f, 指令 *q, const char *因)
{
    if (汇_失败) return;           /* 只报第一条，后面都是连锁反应 */
    汇_失败 = 1;
    fprintf(stderr, "原生后端还不支持：%s\n", 因);
    if (f) fprintf(stderr, "  函数：%.*s\n", f->名长, f->名);
    if (q) fprintf(stderr, "  指令：%s\n", 指名[q->码]);
}

/* ---- 字符串常量：剥引号 + 还转义 ----
 * IR 里存的是源码原文（带引号、带 \n 这种写法），要还原成真正的字节。 */
static unsigned char *汇_解串(const char *s, int n, int *出长)
{
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') { s++; n -= 2; }
    unsigned char *p = (unsigned char *)malloc((size_t)n + 1);
    int 长 = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            char e = s[++i];
            switch (e) {
            case 'n': p[长++] = '\n'; break;
            case 't': p[长++] = '\t'; break;
            case 'r': p[长++] = '\r'; break;
            case '0': p[长++] = '\0'; break;
            case '"': p[长++] = '"';  break;
            case '\\': p[长++] = '\\'; break;
            default: p[长++] = (unsigned char)e; break;
            }
        } else p[长++] = (unsigned char)s[i];
    }
    *出长 = 长;
    return p;
}

static int 汇_串号(const char *文, int 长)
{
    int 实长 = 0;
    unsigned char *字节 = 汇_解串(文, 长, &实长);
    for (int i = 0; i < 汇_串数; i++) {
        if (汇_串[i].长 == 实长 && memcmp(汇_串[i].字节, 字节, (size_t)实长) == 0) {
            free(字节);
            return i;
        }
    }
    if (汇_串数 >= 汇_最多串) { free(字节); return 0; }
    汇_串[汇_串数].字节 = 字节;
    汇_串[汇_串数].长 = 实长;
    return 汇_串数++;
}

/* ---- 运行时：全是裸 syscall ----
 *
 *  简_写(rdi=指针, rsi=长度)          写 stdout
 *  简_打整(rdi=有符号整数)            十进制，不换行
 *  简_换行()                          一个 \n
 *  简_幂(rdi=底, rsi=指数)            rax = 底^指数（指数 < 0 得 0）
 *  简_分配(rdi=字节数)                rax = 一段堆，1MB 一次要够，不释放
 *  简_拼(rdi=串1, rsi=长1, rdx=串2, rcx=长2)   rax=新串指针, rdx=新串长度
 *
 *  堆为什么不回收：语言层目前也没有 `释放`，跟 C 后端里 图/声音 的处理一致。
 *  一个编译产物活几秒，1MB 够用；真要更多会再 mmap 一块。
 */
static void 写运行时汇编(void)
{
    fputs(
"/* ---- 运行时 ---- */\n"
"简_写:\n"
"    movq %rsi, %rdx\n"
"    movq %rdi, %rsi\n"
"    movl $1, %edi\n"
"    movl $1, %eax\n"
"    syscall\n"
"    ret\n"
"\n"
"简_打整:\n"
"    pushq %rbp\n"
"    movq %rsp, %rbp\n"
"    subq $48, %rsp\n"
"    movq %rdi, %rax\n"
"    leaq -1(%rbp), %rsi\n"
"    xorq %rcx, %rcx\n"
"    movq $10, %r8\n"
"    xorq %r9, %r9\n"
"    testq %rax, %rax\n"
"    jns .Lnum_start\n"
"    negq %rax\n"
"    movq $1, %r9\n"
".Lnum_start:\n"
"    testq %rax, %rax\n"
"    jnz .Lnum_loop\n"
"    decq %rsi\n"
"    movb $48, (%rsi)\n"
"    incq %rcx\n"
"    jmp .Lnum_sign\n"
".Lnum_loop:\n"
"    xorq %rdx, %rdx\n"
"    divq %r8\n"
"    addb $48, %dl\n"
"    decq %rsi\n"
"    movb %dl, (%rsi)\n"
"    incq %rcx\n"
"    testq %rax, %rax\n"
"    jnz .Lnum_loop\n"
".Lnum_sign:\n"
"    testq %r9, %r9\n"
"    jz .Lnum_out\n"
"    decq %rsi\n"
"    movb $45, (%rsi)\n"
"    incq %rcx\n"
".Lnum_out:\n"
"    movq %rsi, %rdi\n"
"    movq %rcx, %rsi\n"
"    call 简_写\n"
"    leave\n"
"    ret\n"
"\n"
"简_换行:\n"
"    leaq .Lnl(%rip), %rdi\n"
"    movq $1, %rsi\n"
"    jmp 简_写\n"
"\n"
"简_幂:\n"
"    movq $1, %rax\n"
"    testq %rsi, %rsi\n"
"    jle .Lpow_done\n"
"    testq %rax, %rax\n"
".Lpow_loop:\n"
"    testq $1, %rsi\n"
"    jz .Lpow_skip\n"
"    imulq %rdi, %rax\n"
".Lpow_skip:\n"
"    imulq %rdi, %rdi\n"
"    shrq $1, %rsi\n"
"    jnz .Lpow_loop\n"
".Lpow_done:\n"
"    ret\n"
"\n"
"/* 堆：顶/尾两个全局。第一次用时 mmap 1MB，之后只挪指针。 */\n"
"简_分配:\n"
"    movq %rdi, %rcx\n"
"    movq 简_堆顶(%rip), %rax\n"
"    leaq (%rax,%rcx), %rdx\n"
"    cmpq 简_堆尾(%rip), %rdx\n"
"    jbe .Lalloc_ok\n"
"    pushq %rdi\n"
"    xorq %rdi, %rdi\n"
"    movq $1048576, %rsi\n"
"    movq $3, %rdx\n"
"    movq $34, %r10\n"
"    movq $-1, %r8\n"
"    xorq %r9, %r9\n"
"    movl $9, %eax\n"
"    syscall\n"
"    popq %rdi\n"
"    movq %rax, 简_堆顶(%rip)\n"
"    movq %rax, %rdx\n"
"    addq $1048576, %rdx\n"
"    movq %rdx, 简_堆尾(%rip)\n"
"    leaq (%rax,%rdi), %rdx\n"
"    movq %rdi, %rcx\n"
".Lalloc_ok:\n"
"    movq %rdx, 简_堆顶(%rip)\n"
"    ret\n"
"\n"
"/* 简_拼(rdi=串1, rsi=长1, rdx=串2, rcx=长2) */\n"
"简_拼:\n"
"    pushq %rbp\n"
"    movq %rsp, %rbp\n"
"    pushq %rbx\n"
"    pushq %r12\n"
"    pushq %r13\n"
"    pushq %r14\n"
"    movq %rdi, %rbx\n"
"    movq %rsi, %r12\n"
"    movq %rdx, %r13\n"
"    movq %rcx, %r14\n"
"    leaq 1(%rsi,%rcx), %rdi\n"
"    call 简_分配\n"
"    movq %rax, %rdi\n"
"    movq %rax, %r8\n"
"    movq %rbx, %rsi\n"
"    movq %r12, %rcx\n"
"    rep movsb\n"
"    movq %r13, %rsi\n"
"    movq %r14, %rcx\n"
"    rep movsb\n"
"    movb $0, (%rdi)\n"
"    movq %r8, %rax\n"
"    movq %r12, %rdx\n"
"    addq %r14, %rdx\n"
"    popq %r14\n"
"    popq %r13\n"
"    popq %r12\n"
"    popq %rbx\n"
"    leave\n"
"    ret\n"
"\n"
"    .section .rodata\n"
".Lnl: .byte 10\n"
"\n"
"/* 堆的两个游标。放 .bss：不占文件大小，加载时是 0 */\n"
"    .bss\n"
"    .align 8\n"
"简_堆顶: .skip 8\n"
"简_堆尾: .skip 8\n"
"    .text\n\n", 出);
}

/* ---- 单条指令 ---- */
static void 汇_指令(中间函数 *f, int 序, 指令 *q)
{
    int 果 = q->结果;
    int 甲 = q->甲, 乙 = q->乙;

#define 装(值号, 寄) fprintf(出, "    movq %d(%%rbp), %s\n", 槽(值号), 寄)
#define 存(值号, 寄) fprintf(出, "    movq %s, %d(%%rbp)\n", 寄, 槽(值号))
#define 标(n) fprintf(出, ".L%d_%d", 序, (n))

    switch (q->码) {

    case 指_常量: {
        类型 *t = q->型;
        if (t && t->种 == 型_字符串) {
            int 号 = 汇_串号(q->文, q->文长);
            fprintf(出, "    leaq .LSTR%d(%%rip), %%rax\n", 号);
            存(果, "%rax");
            fprintf(出, "    movq $%d, %d(%%rbp)\n", 汇_串[号].长, 槽(果) + 8);
        } else if (t && t->种 == 型_布尔) {
            int 真 = (q->文长 == 3 && memcmp(q->文, "真", 3) == 0);
            fprintf(出, "    movq $%d, %d(%%rbp)\n", 真, 槽(果));
            fprintf(出, "    movq $0, %d(%%rbp)\n", 槽(果) + 8);
        } else if (t && t->种 == 型_整数) {
            fprintf(出, "    movq $%.*s, %d(%%rbp)\n", q->文长, q->文, 槽(果));
            fprintf(出, "    movq $0, %d(%%rbp)\n", 槽(果) + 8);
        } else {
            汇_坏(f, q, "这种常量类型");
        }
        return;
    }

    case 指_载:
    case 指_移动: case 指_复制: case 指_借用: case 指_可变借用:
        /* 十六字节整块搬：低 8 是值/指针，高 8 是字符串长度（别的类型用不上） */
        装(甲, "%rax"); 存(果, "%rax");
        fprintf(出, "    movq %d(%%rbp), %%rax\n", 槽(甲) + 8);
        fprintf(出, "    movq %%rax, %d(%%rbp)\n", 槽(果) + 8);
        return;

    case 指_存:
        装(乙, "%rax"); 存(甲, "%rax");
        fprintf(出, "    movq %d(%%rbp), %%rax\n", 槽(乙) + 8);
        fprintf(出, "    movq %%rax, %d(%%rbp)\n", 槽(甲) + 8);
        return;

    case 指_空操作:
        return;

    /* ---- 整数二元 ---- */
    case 指_加: case 指_减: case 指_乘:
    case 指_位与: case 指_位或: case 指_位异或:
    case 指_左移: case 指_右移: {
        static const char *op[] = { "addq", "subq", "imulq", "andq", "orq", "xorq", "shlq", "shrq" };
        int k = 0;
        switch (q->码) {
        case 指_加: k = 0; break;
        case 指_减: k = 1; break;
        case 指_乘: k = 2; break;
        case 指_位与: k = 3; break;
        case 指_位或: k = 4; break;
        case 指_位异或: k = 5; break;
        case 指_左移: k = 6; break;
        default: k = 7; break;
        }
        装(甲, "%rax");
        if (k >= 6) {              /* 移位量必须在 %cl */
            装(乙, "%rcx");
            fprintf(出, "    %s %%cl, %%rax\n", op[k]);
        } else {
            fprintf(出, "    %s %d(%%rbp), %%rax\n", op[k], 槽(乙));
        }
        存(果, "%rax");
        return;
    }

    case 指_整除: case 指_取余: {
        装(甲, "%rax");
        装(乙, "%rcx");
        fputs("    cqto\n", 出);
        fputs("    idivq %rcx\n", 出);
        if (q->码 == 指_取余) 存(果, "%rdx");
        else 存(果, "%rax");
        return;
    }

    case 指_除:
        /* 真除法：`/` 在简言里永远是浮点除法，这里没有浮点，只能报错。
         * 整数相除请写 `//`。 */
        汇_坏(f, q, "/ （浮点除法）—— 用 // 做整除，浮点支持还没做");
        return;

    case 指_乘方:
        装(甲, "%rdi");
        装(乙, "%rsi");
        fputs("    call 简_幂\n", 出);
        存(果, "%rax");
        return;

    /* ---- 比较 ---- */
    case 指_等于: case 指_不等:
    case 指_小于: case 指_大于: case 指_小于等: case 指_大于等: {
        static const char *cc[] = { "sete", "setne", "setl", "setg", "setle", "setge" };
        int k = 0;
        switch (q->码) {
        case 指_等于: k = 0; break;
        case 指_不等: k = 1; break;
        case 指_小于: k = 2; break;
        case 指_大于: k = 3; break;
        case 指_小于等: k = 4; break;
        default: k = 5; break;
        }
        装(甲, "%rax");
        装(乙, "%rcx");
        fputs("    cmpq %rcx, %rax\n", 出);
        fprintf(出, "    %s %%al\n", cc[k]);
        fputs("    movzbq %al, %rax\n", 出);
        存(果, "%rax");
        return;
    }

    case 指_非:
        装(甲, "%rax");
        fputs("    testq %rax, %rax\n", 出);
        fputs("    sete %al\n", 出);
        fputs("    movzbq %al, %rax\n", 出);
        存(果, "%rax");
        return;

    case 指_负:
        装(甲, "%rax");
        fputs("    negq %rax\n", 出);
        存(果, "%rax");
        return;

    case 指_取反:
        装(甲, "%rax");
        fputs("    notq %rax\n", 出);
        存(果, "%rax");
        return;

    /* ---- 字符串 ---- */
    case 指_长度:
        /* #字符串 → 长度就存在槽的高 8 字节里，取出来即可 */
        fprintf(出, "    movq %d(%%rbp), %%rax\n", 槽(甲) + 8);
        存(果, "%rax");
        return;

    case 指_拼接: {
        甲 = q->甲; 乙 = q->乙;
        fprintf(出, "    movq %d(%%rbp), %%rdi\n", 槽(甲));
        fprintf(出, "    movq %d(%%rbp), %%rsi\n", 槽(甲) + 8);
        fprintf(出, "    movq %d(%%rbp), %%rdx\n", 槽(乙));
        fprintf(出, "    movq %d(%%rbp), %%rcx\n", 槽(乙) + 8);
        fputs("    call 简_拼\n", 出);
        存(果, "%rax");
        fprintf(出, "    movq %%rdx, %d(%%rbp)\n", 槽(果) + 8);
        return;
    }

    /* ---- 控制流 ---- */
    case 指_跳:
        fprintf(出, "    jmp .L%d_%d\n", 序, 甲);
        return;

    case 指_条件跳:
        fprintf(出, "    cmpq $0, %d(%%rbp)\n", 槽(甲));
        fprintf(出, "    jne .L%d_%d\n", 序, 乙);
        fprintf(出, "    jmp .L%d_%d\n", 序, q->丙);
        return;

    case 指_返回:
        if (甲 >= 0) {
            装(甲, "%rax");
            fprintf(出, "    movq %d(%%rbp), %%rdx\n", 槽(甲) + 8);
            类型 *rt = f->型 ? f->型->返回 : NULL;
            if (rt && rt->种 == 型_字符串)
                fprintf(出, "    movq %d(%%rbp), %%rdx\n", 槽(甲) + 8);
        } else {
            fputs("    xorl %eax, %eax\n", 出);
        }
        fputs("    leave\n    ret\n", 出);
        return;

    case 指_转换: {
        类型 *到 = q->型;
        类型 *从 = 值型(f, 甲);
        if (到 && (到->种 == 型_整数 || 到->种 == 型_布尔) &&
            从 && (从->种 == 型_整数 || 从->种 == 型_布尔)) {
            装(甲, "%rax"); 存(果, "%rax");
            return;
        }
        汇_坏(f, q, "这种类型转换");
        return;
    }

    /* ---- 调用 ---- */
    case 指_调用: {
        int 打印 = (q->文 && q->文长 == 6 && memcmp(q->文, "打印", 6) == 0);
        int 打印行 = (q->文 && q->文长 == 9 && memcmp(q->文, "打印行", 9) == 0);
        if (打印 || 打印行) {
            for (int i = 0; i < q->多量; i++) {
                int v = q->多[i];
                类型 *t = 值型(f, v);
                if (t && t->种 == 型_字符串) {
                    fprintf(出, "    movq %d(%%rbp), %%rdi\n", 槽(v));
                    fprintf(出, "    movq %d(%%rbp), %%rsi\n", 槽(v) + 8);
                    fputs("    call 简_写\n", 出);
                } else if (t && t->种 == 型_布尔) {
                    /* 真/假 是两个字，各自一个标号 */
                    fprintf(出, "    cmpq $0, %d(%%rbp)\n", 槽(v));
                    fprintf(出, "    je .Lbool%d_f\n", 序 * 1000 + v);
                    fputs("    leaq .LTRUE(%rip), %rdi\n    movq $3, %rsi\n    call 简_写\n", 出);
                    fprintf(出, "    jmp .Lbool%d_e\n.Lbool%d_f:\n", 序 * 1000 + v, 序 * 1000 + v);
                    fputs("    leaq .LFALSE(%rip), %rdi\n    movq $3, %rsi\n    call 简_写\n", 出);
                    fprintf(出, ".Lbool%d_e:\n", 序 * 1000 + v);
                } else if (t && t->种 == 型_整数) {
                    fprintf(出, "    movq %d(%%rbp), %%rdi\n", 槽(v));
                    fputs("    call 简_打整\n", 出);
                } else {
                    汇_坏(f, q, "打印这种类型");
                    return;
                }
            }
            if (打印行) fputs("    call 简_换行\n", 出);
            if (果 >= 0) fprintf(出, "    movq $0, %d(%%rbp)\n", 槽(果));
            return;
        }

        中间函数 *g = 找函数(q->文, q->文长);
        if (!g) { 汇_坏(f, q, "调用了未知函数"); return; }
        if (q->多量 > 6) { 汇_坏(f, q, "实参超过 6 个"); return; }
        static const char *寄[] = { "%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9" };
        for (int i = 0; i < q->多量; i++) {
            类型 *t = 值型(f, q->多[i]);
            if (t && (t->种 == 型_字符串 || t->种 == 型_浮点 || t->种 == 型_组 ||
                      t->种 == 型_映射 || t->种 == 型_命名 || t->种 == 型_声音)) {
                汇_坏(f, q, "这类类型的实参");
                return;
            }
            装(q->多[i], 寄[i]);
        }
        int 序g = 汇_序号(g);
        if (序g < 0) { 汇_坏(f, q, "调用了没编译的函数"); return; }
        fprintf(出, "    call 汇F%d\n", 序g);
        if (果 >= 0) 存(果, "%rax");
        return;
    }

    default:
        汇_坏(f, q, "这条指令");
        return;
    }
#undef 装
#undef 存
#undef 标
}

/* ---- 一个函数 ---- */
static void 写函数汇编(中间函数 *f, int 序)
{
    int 参编[16];
    int 参数 = 参数个数(f, 参编);
    if (参数 > 6) { 汇_坏(f, NULL, "参数超过 6 个"); return; }

    /* 帧要 16 字节对齐：调用时 rsp 必须 %16==0，否则 syscall/SSE 会出事 */
    int 帧 = (f->值数 + 4) * 16;
    帧 = (帧 + 15) & ~15;

    fprintf(出, "\n/* ---- 简言函数：%.*s（%d 块 / %d 条指令）---- */\n",
            f->名长, f->名, f->下个块号, f->指令数);
    fprintf(出, "汇F%d:\n", 序);
    fputs("    pushq %rbp\n    movq %rsp, %rbp\n", 出);
    fprintf(出, "    subq $%d, %%rsp\n", 帧);

    static const char *寄[] = { "%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9" };
    for (int i = 0; i < 参数; i++) {
        类型 *t = 值型(f, 参编[i]);
        if (t && (t->种 == 型_字符串 || t->种 == 型_浮点 || t->种 == 型_组 ||
                  t->种 == 型_映射 || t->种 == 型_命名 || t->种 == 型_声音)) {
            汇_坏(f, NULL, "这类类型的形参");
            return;
        }
        fprintf(出, "    movq %s, %d(%%rbp)\n", 寄[i], 槽(参编[i]));
    }

    for (基本块 *b = f->首块; b; b = b->下) {
        fprintf(出, ".L%d_%d:\n", 序, b->号);
        for (指令 *q = b->头; q; q = q->下) 汇_指令(f, 序, q);
    }

    /* 兜底：块掉到头也没返回，就当返回 0，别让 CPU 跑飞去 */
    fputs("    xorl %eax, %eax\n    leave\n    ret\n", 出);
}

/* ---- 入口 ---- */
static void 写入口汇编(void)
{
    int 主序 = -1;
    for (int i = 0; i < 汇_函数; i++)
        if (汇_函[i]->名长 == 3 && memcmp(汇_函[i]->名, "主", 3) == 0) { 主序 = i; break; }

    fputs("\n/* ---- 入口：_start 直接 syscall exit，不经过 libc ---- */\n", 出);
    fputs("    .globl _start\n_start:\n", 出);
    fputs("    andq $-16, %rsp\n", 出);
    if (主序 >= 0) fprintf(出, "    call 汇F%d\n", 主序);
    else fputs("    xorl %eax, %eax\n", 出);
    fputs("    movslq %eax, %rdi\n    movq $60, %rax\n    syscall\n", 出);
}

/* ---- 字符串池 ---- */
static void 写串池汇编(void)
{
    fputs("\n    .section .rodata\n", 出);
    fputs(".LTRUE:  .byte 231,156,159\n", 出);   /* 真 */
    fputs(".LFALSE: .byte 229,129,135\n", 出);   /* 假 */
    for (int i = 0; i < 汇_串数; i++) {
        fprintf(出, ".LSTR%d:\n    .byte ", i);
        if (汇_串[i].长 == 0) { fputs("0", 出); }
        for (int k = 0; k < 汇_串[i].长; k++) {
            if (k) fputc(',', 出);
            fprintf(出, "%d", 汇_串[i].字节[k]);
        }
        fputc('\n', 出);
    }
}

/* ---- 总入口：给简编译.c 调 ---- */
static int 汇_汇编(节点 *根)
{
    (void)根;

    for (中间函数 *f = 模块体.头; f; f = f->下) {
        if (汇_函数 >= 汇_最多函) { 汇_坏(NULL, NULL, "函数太多"); break; }
        汇_函[汇_函数++] = f;
    }

    /* 先把字符串常量收齐，函数体里就能直接引标号了 */
    for (int i = 0; i < 汇_函数; i++) {
        中间函数 *f = 汇_函[i];
        for (基本块 *b = f->首块; b; b = b->下)
            for (指令 *q = b->头; q; q = q->下) {
                if (q->码 == 指_常量 && q->型 && q->型->种 == 型_字符串 && q->文)
                    汇_串号(q->文, q->文长);
            }
    }

    fputs("/* ============================================================\n"
          " *  这份汇编由 简言 原生后端直接生成，不经过 C，也不经过 gcc。\n"
          " *  流程：.jy -> 词法 -> 语法 -> 语义 -> IR -> 这里 -> as -> ld\n"
          " * ============================================================ */\n\n", 出);
    fputs("    .text\n\n", 出);

    写运行时汇编();
    for (int i = 0; i < 汇_函数; i++) 写函数汇编(汇_函[i], i);
    写入口汇编();
    写串池汇编();

    return !汇_失败;
}
