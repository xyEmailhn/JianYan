/* ============================================================================
 *  原生后端 v2：IR → x86-64 汇编，带寄存器分配
 *  ----------------------------------------------------------------------------
 *  v1 是「栈机」：每个值一个栈槽，算一条指令是 载-算-存 三步。
 *  对着机器码一看就知道亏在哪——`x = x + 1` 被写成三条访存。
 *
 *  v2 干两件事：
 *
 *    1. 寄存器分配：活跃变量分析 + 线性扫描。一个值在它活着的那段里待在
 *       一个寄存器里，只有寄存器不够时才溢出回栈。
 *    2. 指令选择：加法走 lea（三地址、不碰标志位），小常数走立即数，
 *       比较直接跟条件跳合起来——`如果 a << b` 不再先算出一个 0/1 再判断。
 *
 *  寄存器怎么分：11 个通用寄存器参与分配，%rax %rcx %rdx 留作手边临时。
 *  留这三个不是随手挑的：idiv 要 rax:rdx，移位要 cl，返回值在 rax。
 *  与其在分配器里到处为它们打补丁，不如干脆不分配。
 *
 *  寄存器不够时溢出到「家槽」——每个值仍然有一个固定的 16 字节槽位，
 *  这样溢出代码就是一句 mov，不需要额外记账。槽也用来在函数调用处
 *  保存 caller-saved 寄存器里的活值。
 * ========================================================================== */

#define 汇_最多串 2048
#define 汇_最多函 512
#define 汇_寄数 11
#define 汇_池数 3

static int 汇_失败 = 0;

typedef struct { unsigned char *字节; int 长; } 汇串项;
static 汇串项 汇_串[汇_最多串];
static int 汇_串数 = 0;

static 中间函数 *汇_函[汇_最多函];
static int 汇_函数 = 0;

static int 汇_标号 = 0;

/* 可分配的 11 个通用寄存器。寄稳=1 表示 callee-saved（被调用者负责保存）。 */
static const char *汇_寄名[汇_寄数] = {
    "%rbx", "%rsi", "%rdi", "%r8", "%r9", "%r10", "%r11", "%r12", "%r13", "%r14", "%r15"
};
static const unsigned char 汇_寄稳[汇_寄数] = { 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1 };

/* 手边临时：%rax %rcx %rdx */
static const char *汇_池名[汇_池数] = { "%rax", "%rcx", "%rdx" };
static int 汇_池占[汇_池数];

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

/* ---- 浮点常量池 ----
 * 按位比较：-0.0 和 0.0 数值相等但位不同，混用会把 -0.0 打印成 0。 */
static double 汇_浮值[汇_最多串];
static int 汇_浮数 = 0;

static int 汇_浮号(double v)
{
    for (int i = 0; i < 汇_浮数; i++)
        if (memcmp(&汇_浮值[i], &v, sizeof(double)) == 0) return i;
    if (汇_浮数 >= 汇_最多串) return 0;
    汇_浮值[汇_浮数] = v;
    return 汇_浮数++;
}

static double 汇_浮文(const char *s, int n)
{
    char b[80];
    int m = n > 0 && n < 79 ? n : 0;
    if (!m) return 0.0;
    memcpy(b, s, (size_t)m);
    b[m] = 0;
    return strtod(b, NULL);
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

/* ============================================================================
 *  运行时：全是裸 syscall
 *  ----------------------------------------------------------------------------
 *  简_写(rdi=指针, rsi=长度)          写 stdout
 *  简_打整(rdi=有符号整数)            十进制，不换行
 *  简_换行()                          一个 \n
 *  简_幂(rdi=底, rsi=指数)            rax = 底^指数（指数 < 0 得 0）
 *  简_分配(rdi=字节数)                rax = 一段堆，1MB 一次要够，不释放
 *  简_拼(rdi=串1, rsi=长1, rdx=串2, rcx=长2)   rax=新串指针, rdx=新串长度
 *
 *  堆为什么不回收：语言层目前也没有 `释放`，跟 C 后端里 图/声音 的处理一致。
 * ========================================================================== */
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
"/* 简_字数(rdi=字节, rsi=长) → rax = 字符数：数非续字节 */\n"
"简_字数:\n"
"    xorq %rax, %rax\n"
"    xorq %rcx, %rcx\n"
".Lcnt_loop:\n"
"    cmpq %rsi, %rcx\n"
"    jge .Lcnt_done\n"
"    movb (%rdi,%rcx), %dl\n"
"    andb $0xc0, %dl\n"
"    cmpb $0x80, %dl\n"
"    je .Lcnt_next\n"
"    incq %rax\n"
".Lcnt_next:\n"
"    incq %rcx\n"
"    jmp .Lcnt_loop\n"
".Lcnt_done:\n"
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
"/* ---- 简_打浮：xmm0 里的 double，按 %g 的样子写出来 ----\n"
" * 6 位有效数字、去尾零；指数 < -4 或 >= 6 走科学计数法。\n"
" * 只用 SSE 和整数除法，不碰 libc。 */\n"
"简_打浮:\n"
"    pushq %rbp\n"
"    movq %rsp, %rbp\n"
"    pushq %rbx\n"
"    pushq %r12\n"
"    pushq %r13\n"
"    pushq %r14\n"
"    subq $128, %rsp\n"
"    leaq -33(%rbp), %rbx\n"
"    leaq -160(%rbp), %r12\n"
"    movq %xmm0, %rax\n"
"    shrq $63, %rax\n"
"    movq %rax, %r13\n"
"    testq %r13, %r13\n"
"    jz .Lpf_pos\n"
"    movq %xmm0, %rax\n"
"    btrq $63, %rax\n"
"    movq %rax, %xmm0\n"
".Lpf_pos:\n"
"    xorpd %xmm1, %xmm1\n"
"    ucomisd %xmm1, %xmm0\n"
"    jne .Lpf_nz\n"
"    movb $48, -34(%rbp)\n"
"    leaq -34(%rbp), %rbx\n"
"    jmp .Lpf_emit\n"
".Lpf_nz:\n"
"    xorl %r9d, %r9d\n"
"    movsd .LCF十(%rip), %xmm2\n"
".Lpf_big:\n"
"    ucomisd %xmm2, %xmm0\n"
"    jb .Lpf_small\n"
"    divsd %xmm2, %xmm0\n"
"    incq %r9\n"
"    jmp .Lpf_big\n"
".Lpf_small:\n"
"    movsd .LCF一(%rip), %xmm2\n"
".Lpf_sm2:\n"
"    ucomisd %xmm2, %xmm0\n"
"    jae .Lpf_norm\n"
"    movsd .LCF十(%rip), %xmm3\n"
"    mulsd %xmm3, %xmm0\n"
"    decq %r9\n"
"    jmp .Lpf_sm2\n"
".Lpf_norm:\n"
"    movsd .LCF十万(%rip), %xmm2\n"
"    mulsd %xmm2, %xmm0\n"
"    cvtsd2si %xmm0, %rax\n"
"    cmpq $1000000, %rax\n"
"    jl .Lpf_dok\n"
"    movq $100000, %rax\n"
"    incq %r9\n"
".Lpf_dok:\n"
"    movl $5, %ecx\n"
"    movq $10, %r8\n"
".Lpf_dig:\n"
"    xorq %rdx, %rdx\n"
"    divq %r8\n"
"    addb $48, %dl\n"
"    movb %dl, (%r12,%rcx)\n"
"    decq %rcx\n"
"    jns .Lpf_dig\n"
"    cmpq $-4, %r9\n"
"    jl .Lpf_exp\n"
"    cmpq $6, %r9\n"
"    jge .Lpf_exp\n"
"    cmpq $0, %r9\n"
"    jl .Lpf_nege\n"
"    movq $5, %r10\n"
".Lpf_a1:\n"
"    cmpq %r9, %r10\n"
"    jle .Lpf_a2\n"
"    cmpb $48, (%r12,%r10)\n"
"    jne .Lpf_a2\n"
"    decq %r10\n"
"    jmp .Lpf_a1\n"
/* 缓冲是往前长的，所以最后写的那个字符在最左边。
 * 先把小数写完、再写小数点、最后写整数部分——顺序反了就成了 25.3。 */
".Lpf_a2:\n"
"    cmpq %r9, %r10\n"
"    jle .Lpf_a3\n"
"    movq %r10, %rcx\n"
".Lpf_a4:\n"
"    movb (%r12,%rcx), %al\n"
"    decq %rbx\n"
"    movb %al, (%rbx)\n"
"    decq %rcx\n"
"    cmpq %r9, %rcx\n"
"    jg .Lpf_a4\n"
"    movb $46, %al\n"
"    decq %rbx\n"
"    movb %al, (%rbx)\n"
".Lpf_a3:\n"
"    movq %r9, %rcx\n"
".Lpf_a5:\n"
"    movb (%r12,%rcx), %al\n"
"    decq %rbx\n"
"    movb %al, (%rbx)\n"
"    decq %rcx\n"
"    jns .Lpf_a5\n"
"    jmp .Lpf_emit\n"
".Lpf_nege:\n"
"    movq $5, %r10\n"
".Lpf_b1:\n"
"    cmpb $48, (%r12,%r10)\n"
"    jne .Lpf_b2\n"
"    decq %r10\n"
"    jns .Lpf_b1\n"
".Lpf_b2:\n"
"    movq %r10, %rcx\n"
".Lpf_b3:\n"
"    movb (%r12,%rcx), %al\n"
"    decq %rbx\n"
"    movb %al, (%rbx)\n"
"    decq %rcx\n"
"    jns .Lpf_b3\n"
"    movq %r9, %rcx\n"
"    negq %rcx\n"
"    decq %rcx\n"
".Lpf_b4:\n"
"    testq %rcx, %rcx\n"
"    jle .Lpf_b5\n"
"    movb $48, %al\n"
"    decq %rbx\n"
"    movb %al, (%rbx)\n"
"    decq %rcx\n"
"    jmp .Lpf_b4\n"
".Lpf_b5:\n"
"    movb $46, %al\n"
"    decq %rbx\n"
"    movb %al, (%rbx)\n"
"    movb $48, %al\n"
"    decq %rbx\n"
"    movb %al, (%rbx)\n"
"    jmp .Lpf_emit\n"
".Lpf_exp:\n"
"    movq $5, %r10\n"
".Lpf_c1:\n"
"    cmpb $48, (%r12,%r10)\n"
"    jne .Lpf_c2\n"
"    decq %r10\n"
"    jns .Lpf_c1\n"
/* 指数形式：先写指数（贴着右边），再 'e'、'.'、尾数，
 * 最后写的尾数落在最左边。 */
".Lpf_c2:\n"
"    movq %r9, %r11\n"
"    testq %r11, %r11\n"
"    jns .Lpf_c6\n"
"    negq %r11\n"
".Lpf_c6:\n"
"    movq %r11, %rax\n"
"    movq $10, %r8\n"
"    xorq %rdx, %rdx\n"
"    divq %r8\n"
"    addb $48, %dl\n"
"    decq %rbx\n"
"    movb %dl, (%rbx)\n"
"    movq %rax, %rcx\n"
"    testq %rcx, %rcx\n"
"    jnz .Lpf_c7\n"
"    movb $48, %al\n"
"    decq %rbx\n"
"    movb %al, (%rbx)\n"
"    jmp .Lpf_c8\n"
".Lpf_c7:\n"
"    xorq %rdx, %rdx\n"
"    movq %rcx, %rax\n"
"    divq %r8\n"
"    addb $48, %dl\n"
"    decq %rbx\n"
"    movb %dl, (%rbx)\n"
"    movq %rax, %rcx\n"
"    testq %rcx, %rcx\n"
"    jnz .Lpf_c7\n"
".Lpf_c8:\n"
"    testq %r9, %r9\n"
"    jns .Lpf_c9\n"
"    movb $45, %al\n"
"    jmp .Lpf_c10\n"
".Lpf_c9:\n"
"    movb $43, %al\n"
".Lpf_c10:\n"
"    decq %rbx\n"
"    movb %al, (%rbx)\n"
"    movb $101, %al\n"
"    decq %rbx\n"
"    movb %al, (%rbx)\n"
/* 小数点在首位数之后：先写 dig[r10..1]，再 '.'，最后 dig[0]。
 * 反过来的话，'.' 会被顶到最左边，变成 123457.e+06。 */
"    cmpq $0, %r10\n"
"    jle .Lpf_c13\n"
"    movq %r10, %rcx\n"
".Lpf_c12:\n"
"    movb (%r12,%rcx), %al\n"
"    decq %rbx\n"
"    movb %al, (%rbx)\n"
"    decq %rcx\n"
"    cmpq $0, %rcx\n"
"    jg .Lpf_c12\n"
"    movb $46, %al\n"
"    decq %rbx\n"
"    movb %al, (%rbx)\n"
".Lpf_c13:\n"
"    movb (%r12), %al\n"
"    decq %rbx\n"
"    movb %al, (%rbx)\n"
"    jmp .Lpf_emit\n"
".Lpf_emit:\n"
"    testq %r13, %r13\n"
"    jz .Lpf_go\n"
"    movb $45, %al\n"
"    decq %rbx\n"
"    movb %al, (%rbx)\n"
".Lpf_go:\n"
"    movq %rbx, %rdi\n"
"    leaq -33(%rbp), %rsi\n"
"    subq %rbx, %rsi\n"
"    call 简_写\n"
"    addq $128, %rsp\n"
"    popq %r14\n"
"    popq %r13\n"
"    popq %r12\n"
"    popq %rbx\n"
"    leave\n"
"    ret\n"
"\n"
"    .section .rodata\n"
".Lnl: .byte 10\n"
"    .align 8\n"
".LCF一: .quad 0x3ff0000000000000\n"
".LCF十: .quad 0x4024000000000000\n"
".LCF十万: .quad 0x40f86a0000000000\n"
"\n"
"/* 堆的两个游标。放 .bss：不占文件大小，加载时是 0 */\n"
"    .bss\n"
"    .align 8\n"
"简_堆顶: .skip 8\n"
"简_堆尾: .skip 8\n"
"    .text\n\n", 出);
}

/* ============================================================================
 *  第一部分：分析
 *  ----------------------------------------------------------------------------
 *  四步，一步不能少：
 *    1. 把块按发射顺序摊平，每条指令占两个「位置」——用在前，定义在后。
 *       位置差半个是故意的：`d = a + b` 里 a、b 在第 i 步用掉，d 在同一步
 *       才出生，这样「a 死了，d 用 a 的寄存器」就是天然成立的。
 *    2. 算每块的 use/def，再对 CFG 做数据流迭代，得 live-in / live-out。
 *    3. 每个值合成一个保守的区间 [第一次出现, 最后一次出现]。
 *    4. 线性扫描：按起点排序扫过去，寄存器不够就溢出「最晚死」的那个。
 * ========================================================================== */

typedef struct {
    中间函数 *f;
    int 值数;

    int *槽;              /* 值 → 家槽偏移 */
    int *寄;              /* 值 → 寄存器下标；-1 = 在栈上 */
    int *用次;            /* 值 → 被用了几次 */
    指令 **定义;          /* 值 → 定义它的那条指令 */
    unsigned char *常值;  /* 值 → 是「只定义一次的整数/布尔常量」 */

    int 块数;
    基本块 **块;          /* 发射顺序 */
    int *块起;
    int *块条;
    int 指令总;

    int 字;
    unsigned long *活入, *活出;

    int *区起, *区止;
    unsigned char *跨调;

    int 稳用[汇_寄数];
    int 稳数;
    基本块 **号块;        /* 块号 → 块 */
} 函数态;

/* 当前块物理上的下一块：它就在下面，跳过去的那条 jmp 可以省掉 */
static 基本块 *汇_下块 = NULL;

static 函数态 汇_态;
static int 汇_槽基 = 0;

#define 汇_有位(p, i) (((p)[(i) >> 6] >> ((i) & 63)) & 1UL)
#define 汇_置位(p, i) ((p)[(i) >> 6] |= 1UL << ((i) & 63))
#define 汇_清位(p, i) ((p)[(i) >> 6] &= ~(1UL << ((i) & 63)))

static int 汇_槽(int v) { return -(汇_槽基 + 16 * (v + 1)); }

/* 整数/布尔常量：不占寄存器，用到时现塞立即数 */
#define 汇_是常(v) ((v) >= 0 && (v) < 汇_态.值数 && 汇_态.常值 && 汇_态.常值[(v)])

/* 这条指令的第 i 个操作数是不是「值」（甲/乙/丙 里混着块号） */
static int 汇_操作数(指令 *q, int i)
{
    int v = (i == 0) ? q->甲 : (i == 1) ? q->乙 : q->丙;
    if (v < 0) return -1;
    switch (q->码) {
    case 指_跳:      return -1;                        /* 甲 = 块号 */
    case 指_条件跳:  return (i == 0) ? v : -1;         /* 乙 丙 = 块号 */
    case 指_存:      return (i == 0) ? -1 : v;         /* 甲 是目的地，不是用 */
    default:         return v;
    }
}

/* 纯指令：没有副作用，结果没人用就能整条扔掉 */
static int 汇_纯(int 码)
{
    switch (码) {
    case 指_调用: case 指_调用方法: case 指_拼接:
    case 指_构造组: case 指_构造映射: case 指_构造结构: case 指_构造变体:
    case 指_发送: case 指_接收: case 指_建通道: case 指_关通道: case 指_等待:
    case 指_存: case 指_存字段: case 指_存下标: case 指_丢弃:
        return 0;
    default:
        return 1;
    }
}

static int 汇_两字(类型 *t) { return t && t->种 == 型_字符串; }

typedef struct { int 值, 起, 止; } 汇区间;

static int 汇_比起(const void *a, const void *b)
{
    const 汇区间 *x = (const 汇区间 *)a, *y = (const 汇区间 *)b;
    if (x->起 != y->起) return x->起 - y->起;
    return x->值 - y->值;
}

/* ---- 块结构：后继、编号映射 ---- */
static int 汇_后继(函数态 *s, 基本块 *bb, 基本块 **出, int *号到序)
{
    int 数 = 0;
    if (bb->尾) {
        指令 *末 = bb->尾;
        if (末->码 == 指_跳) {
            if (末->甲 >= 0) 出[数++] = 号到序[末->甲] > 0 ? s->块[号到序[末->甲] - 1] : NULL;
        } else if (末->码 == 指_条件跳) {
            if (末->乙 >= 0) 出[数++] = 号到序[末->乙] > 0 ? s->块[号到序[末->乙] - 1] : NULL;
            if (末->丙 >= 0) 出[数++] = 号到序[末->丙] > 0 ? s->块[号到序[末->丙] - 1] : NULL;
        } else if (末->码 != 指_返回) {
            出[数++] = bb->下;
        }
    } else {
        出[数++] = bb->下;
    }
    return 数;
}

static void 汇_算活性(void)
{
    函数态 *s = &汇_态;
    int 值数 = s->值数, 块数 = s->块数;
    s->字 = (值数 + 63) / 64;
    if (s->字 < 1) s->字 = 1;

    int 号数 = s->f->下个块号 + 2;
    int *号到序 = (int *)calloc((size_t)号数, sizeof(int));
    for (int b = 0; b < 块数; b++) {
        int 号 = s->块[b]->号;
        if (号 >= 0 && 号 < 号数) 号到序[号] = b + 1;   /* 0 表示「没有」 */
    }

    unsigned long *用集 = (unsigned long *)calloc((size_t)(块数 * s->字), sizeof(unsigned long));
    unsigned long *定集 = (unsigned long *)calloc((size_t)(块数 * s->字), sizeof(unsigned long));
    s->活入 = (unsigned long *)calloc((size_t)(块数 * s->字), sizeof(unsigned long));
    s->活出 = (unsigned long *)calloc((size_t)(块数 * s->字), sizeof(unsigned long));

    for (int b = 0; b < 块数; b++) {
        unsigned long *U = 用集 + (size_t)b * s->字;
        unsigned long *D = 定集 + (size_t)b * s->字;
        for (指令 *q = s->块[b]->头; q; q = q->下) {
            for (int i = 0; i < 3; i++) {
                int v = 汇_操作数(q, i);
                if (v >= 0 && v < 值数 && !汇_有位(D, v)) 汇_置位(U, v);
            }
            for (int i = 0; i < q->多量; i++) {
                int v = q->多[i];
                if (v >= 0 && v < 值数 && !汇_有位(D, v)) 汇_置位(U, v);
            }
            if (q->结果 >= 0 && q->结果 < 值数) 汇_置位(D, q->结果);
            if (q->码 == 指_存 && q->甲 >= 0 && q->甲 < 值数) 汇_置位(D, q->甲);
        }
    }

    int 变了 = 1;
    while (变了) {
        变了 = 0;
        for (int b = 块数 - 1; b >= 0; b--) {
            基本块 *bb = s->块[b];
            unsigned long *O = s->活出 + (size_t)b * s->字;
            unsigned long *I = s->活入 + (size_t)b * s->字;
            unsigned long *U = 用集 + (size_t)b * s->字;
            unsigned long *D = 定集 + (size_t)b * s->字;

            基本块 *后[2];
            int 后数 = 汇_后继(s, bb, 后, 号到序);

            unsigned long *新 = (unsigned long *)calloc((size_t)s->字, sizeof(unsigned long));
            for (int k = 0; k < 后数; k++) {
                if (!后[k]) continue;
                int j = 号到序[后[k]->号] - 1;
                if (j < 0) continue;
                unsigned long *J = s->活入 + (size_t)j * s->字;
                for (int w = 0; w < s->字; w++) 新[w] |= J[w];
            }
            int 动 = 0;
            for (int w = 0; w < s->字; w++) if (新[w] != O[w]) { O[w] = 新[w]; 动 = 1; }
            for (int w = 0; w < s->字; w++) 新[w] &= ~D[w];
            for (int w = 0; w < s->字; w++) 新[w] |= U[w];
            for (int w = 0; w < s->字; w++) if (新[w] != I[w]) { I[w] = 新[w]; 动 = 1; }
            if (动) 变了 = 1;
            free(新);
        }
    }

    free(号到序);
    free(用集);
    free(定集);
}

/* 合成区间：保守地取「最早出现」到「最晚出现」。中间死掉的那段也占着，
 * 但正确性绝对没问题，代价只是偶尔多溢出一次。 */
static void 汇_算区间(void)
{
    函数态 *s = &汇_态;
    int 值数 = s->值数, 块数 = s->块数;
    s->区起 = (int *)malloc(sizeof(int) * (size_t)值数);
    s->区止 = (int *)malloc(sizeof(int) * (size_t)值数);
    for (int v = 0; v < 值数; v++) { s->区起[v] = 0x7fffffff; s->区止[v] = -1; }

    int 位 = 0;
    for (int b = 0; b < 块数; b++) {
        基本块 *bb = s->块[b];
        unsigned long *I = s->活入 + (size_t)b * s->字;
        unsigned long *O = s->活出 + (size_t)b * s->字;

        if (bb->头 && 2 * 位 < 0x7fffffff) {
            for (int v = 0; v < 值数; v++)
                if (汇_有位(I, v) && 2 * 位 < s->区起[v]) s->区起[v] = 2 * 位;
        }
        int 末位 = 位;
        for (指令 *q = bb->头; q; q = q->下) {
            for (int i = 0; i < 3; i++) {
                int v = 汇_操作数(q, i);
                if (v >= 0 && v < 值数 && 2 * 位 > s->区止[v]) s->区止[v] = 2 * 位;
            }
            for (int i = 0; i < q->多量; i++) {
                int v = q->多[i];
                if (v >= 0 && v < 值数 && 2 * 位 > s->区止[v]) s->区止[v] = 2 * 位;
            }
            if (q->结果 >= 0 && q->结果 < 值数 && 2 * 位 + 1 < s->区起[q->结果])
                s->区起[q->结果] = 2 * 位 + 1;
            if (q->码 == 指_存 && q->甲 >= 0 && q->甲 < 值数 && 2 * 位 + 1 < s->区起[q->甲])
                s->区起[q->甲] = 2 * 位 + 1;
            末位 = 位;
            位++;
        }
        if (bb->尾) {
            for (int v = 0; v < 值数; v++)
                if (汇_有位(O, v) && 2 * 末位 + 1 > s->区止[v]) s->区止[v] = 2 * 末位 + 1;
        }
    }
    for (int v = 0; v < 值数; v++) {
        if (s->区止[v] < 0) { s->区起[v] = 0; s->区止[v] = -1; }   /* 没人用 */
        if (s->区起[v] > s->区止[v]) s->区起[v] = s->区止[v];
        /* 浮点值不分配通用寄存器：空区间，分配器自然跳过它 */
        类型 *t = 值型(s->f, v);
        if (t && t->种 == 型_浮点) { s->区起[v] = 0x7fffffff; s->区止[v] = -1; }
    }
}

/* 哪些值活过了某条调用——它们不能懒在 caller-saved 里 */
static void 汇_算跨调(void)
{
    函数态 *s = &汇_态;
    s->跨调 = (unsigned char *)calloc((size_t)s->值数, 1);
    int 位 = 0;
    for (int b = 0; b < s->块数; b++) {
        for (指令 *q = s->块[b]->头; q; q = q->下) {
            int 是调 = (q->码 == 指_调用) || (q->码 == 指_调用方法) ||
                       (q->码 == 指_乘方) || (q->码 == 指_拼接);
            if (是调) {
                for (int v = 0; v < s->值数; v++)
                    if (s->区起[v] <= 2 * 位 && s->区止[v] >= 2 * 位 + 1) s->跨调[v] = 1;
            }
            位++;
        }
    }
}

/* ============================================================================
 *  第二部分：线性扫描
 * ========================================================================== */
static void 汇_分配(void)
{
    函数态 *s = &汇_态;
    int 值数 = s->值数;

    s->寄 = (int *)malloc(sizeof(int) * (size_t)值数);
    for (int v = 0; v < 值数; v++) s->寄[v] = -1;

    汇区间 *区 = (汇区间 *)malloc(sizeof(汇区间) * (size_t)值数);
    int 区数 = 0;
    for (int v = 0; v < 值数; v++) {
        if (s->区止[v] < 0) continue;                 /* 没人用，不占寄存器 */
        if (汇_是常(v)) continue;                     /* 常量不占：用到时现塞立即数 */
        区[区数].值 = v; 区[区数].起 = s->区起[v]; 区[区数].止 = s->区止[v];
        区数++;
    }
    qsort(区, (size_t)区数, sizeof(汇区间), 汇_比起);

    int 活区[1024]; int 活数 = 0;
    int 空[汇_寄数];
    for (int i = 0; i < 汇_寄数; i++) 空[i] = 1;

    for (int i = 0; i < 区数; i++) {
        int 起 = 区[i].起;

        for (int k = 0; k < 活数; ) {
            if (区[活区[k]].止 < 起) {
                int r = s->寄[区[活区[k]].值];
                if (r >= 0) 空[r] = 1;
                活区[k] = 活区[--活数];
            } else k++;
        }

        int 选 = -1;

        /* 跟「第一个操作数」同寄存器：`x = x * 31` 这种就地更新，
         * 一旦源和目标同一个寄存器，就成了一条 imulq，连 mov 都省了。
         * 前提是源正好在这条指令上死掉，它的寄存器此刻是空的。 */
        指令 *定 = s->定义[区[i].值];
        if (定) {
            int 源 = 汇_操作数(定, 0);
            if (源 >= 0 && 源 < 值数 && s->寄[源] >= 0 && s->区止[源] < 起) {
                int r = s->寄[源];
                if (空[r] && (!s->跨调[区[i].值] || 汇_寄稳[r])) 选 = r;
            }
        }

        if (选 < 0 && s->跨调[区[i].值])
            for (int r = 0; r < 汇_寄数; r++) if (空[r] && 汇_寄稳[r]) { 选 = r; break; }
        if (选 < 0) for (int r = 0; r < 汇_寄数; r++) if (空[r]) { 选 = r; break; }

        if (选 < 0) {
            int 最 = -1, 最止 = 区[i].止;
            for (int k = 0; k < 活数; k++)
                if (区[活区[k]].止 > 最止) { 最止 = 区[活区[k]].止; 最 = k; }
            if (最 < 0) continue;                     /* 自己就是最晚死的 → 溢出自己 */
            int r = s->寄[区[活区[最]].值];
            s->寄[区[活区[最]].值] = -1;
            s->寄[区[i].值] = r;
            空[r] = 0;
            活区[最] = i;
            continue;
        }

        s->寄[区[i].值] = 选;
        空[选] = 0;
        if (活数 < 1024) 活区[活数++] = i;
    }

    free(区);

    s->稳数 = 0;
    for (int r = 0; r < 汇_寄数; r++) s->稳用[r] = 0;
    for (int v = 0; v < 值数; v++) {
        int r = s->寄[v];
        if (r >= 0 && 汇_寄稳[r]) s->稳用[r] = 1;
    }
    for (int r = 0; r < 汇_寄数; r++) if (s->稳用[r]) s->稳数++;
}

/* ============================================================================
 *  第三部分：发射
 * ========================================================================== */

static const char *汇_池拿(void)
{
    for (int i = 0; i < 汇_池数; i++)
        if (!汇_池占[i]) { 汇_池占[i] = 1; return 汇_池名[i]; }
    return "%rax";
}
static void 汇_池放(const char *r)
{
    for (int i = 0; i < 汇_池数; i++) if (strcmp(汇_池名[i], r) == 0) 汇_池占[i] = 0;
}
static void 汇_池清(void) { 汇_池占[0] = 汇_池占[1] = 汇_池占[2] = 0; }

/* 能直接塞进 imm32 的小整数常量？ */
static int 汇_速常(int v, long long *出值)
{
    if (!汇_是常(v)) return 0;
    指令 *q = 汇_态.定义[v];
    if (!q || !q->文) return 0;
    if (!q->型 || q->型->种 == 型_浮点) return 0;
    if (q->型->种 == 型_布尔) { *出值 = (q->文长 == 3 && memcmp(q->文, "真", 3) == 0); return 1; }
    char buf[64];
    int n = q->文长 < 63 ? q->文长 : 63;
    memcpy(buf, q->文, (size_t)n); buf[n] = 0;
    char *end = NULL;
    long long x = strtoll(buf, &end, 0);
    if (end == buf) return 0;
    if (x < -2147483648LL || x > 2147483647LL) return 0;
    *出值 = x;
    return 1;
}

/* 常量不占寄存器：要用的时候现塞进一个临时 */

/* 把值 v 弄进一个寄存器，返回寄存器名 */
static const char *汇_取(int v)
{
    if (v < 0 || v >= 汇_态.值数) return "%rax";
    if (汇_态.寄[v] >= 0) return 汇_寄名[汇_态.寄[v]];
    long long k;
    if (汇_速常(v, &k)) {
        const char *t = 汇_池拿();
        fprintf(出, "    movq $%lld, %s\n", k, t);
        return t;
    }
    const char *t = 汇_池拿();
    fprintf(出, "    movq %d(%%rbp), %s\n", 汇_槽(v), t);
    return t;
}

/* 把值 v 放进指定寄存器 dst */
static void 汇_进(int v, const char *dst)
{
    long long k;
    if (v >= 0 && v < 汇_态.值数 && 汇_态.寄[v] >= 0) {
        const char *r = 汇_寄名[汇_态.寄[v]];
        if (strcmp(r, dst) != 0) fprintf(出, "    movq %s, %s\n", r, dst);
    } else if (汇_速常(v, &k)) {
        fprintf(出, "    movq $%lld, %s\n", k, dst);
    } else {
        fprintf(出, "    movq %d(%%rbp), %s\n", 汇_槽(v), dst);
    }
}

/* 结果值要写到哪个寄存器；*落 = 1 表示它是溢出的，算完要写回槽 */
static const char *汇_目(int v, int *落)
{
    if (v >= 0 && v < 汇_态.值数 && 汇_态.寄[v] >= 0) {
        *落 = 0;
        return 汇_寄名[汇_态.寄[v]];
    }
    *落 = 0;
    for (int i = 0; i < 汇_池数; i++)
        if (!汇_池占[i]) { 汇_池占[i] = 1; *落 = 1; return 汇_池名[i]; }
    *落 = 1;
    return "%rax";
}

static void 汇_落值(int v, int 落, const char *r)
{
    if (落) fprintf(出, "    movq %s, %d(%%rbp)\n", r, 汇_槽(v));
}

/* 小整数常量？ */
static int 汇_立即(int v, long long *出值)
{
    if (!汇_是常(v)) return 0;
    指令 *q = 汇_态.定义[v];
    if (!q || !q->文 || !q->型 || q->型->种 != 型_整数) return 0;
    char buf[64];
    int n = q->文长 < 63 ? q->文长 : 63;
    memcpy(buf, q->文, (size_t)n); buf[n] = 0;
    char *end = NULL;
    long long x = strtoll(buf, &end, 10);
    if (end == buf) return 0;
    *出值 = x;
    return 1;
}
static int 汇_小常(int v, long long *出值)
{
    long long k;
    if (!汇_立即(v, &k)) return 0;
    if (k < -2147483648LL || k > 2147483647LL) return 0;
    *出值 = k;
    return 1;
}

/* 保存/恢复活过某条指令的 caller-saved 值 */
static void 汇_存跨(int idx)
{
    函数态 *s = &汇_态;
    for (int v = 0; v < s->值数; v++) {
        int r = s->寄[v];
        if (r >= 0 && !汇_寄稳[r] && s->区起[v] <= 2 * idx && s->区止[v] >= 2 * idx + 1)
            fprintf(出, "    movq %s, %d(%%rbp)\n", 汇_寄名[r], 汇_槽(v));
    }
    /* 槽里原来就有的不用管：溢出值本来就待在家槽 */
}
static void 汇_取跨(int idx)
{
    函数态 *s = &汇_态;
    for (int v = 0; v < s->值数; v++) {
        int r = s->寄[v];
        if (r >= 0 && !汇_寄稳[r] && s->区起[v] <= 2 * idx && s->区止[v] >= 2 * idx + 1)
            fprintf(出, "    movq %d(%%rbp), %s\n", 汇_槽(v), 汇_寄名[r]);
    }
}

/* 传参：先把实参落到各自家槽，再从家槽搬进参数寄存器。
 * 为什么不在寄存器之间直接搬：目标寄存器可能正是另一个实参的源，绕一圈最省心。 */
static void 汇_传参(指令 *q, const char **目, int 数)
{
    /* 先看有没有「目的寄存器正好是另一个实参的源」的死结。
     * 有就退一步：全落家槽，再从槽里搬——慢一点，但不会搬错。
     * 没有就直接搬，这是绝大多数情况。 */
    int 冲 = 0;
    for (int i = 0; i < 数 && !冲; i++) {
        if (汇_态.寄[q->多[i]] < 0) continue;
        for (int j = 0; j < 数; j++)
            if (i != j && 汇_态.寄[q->多[i]] >= 0 &&
                strcmp(汇_寄名[汇_态.寄[q->多[i]]], 目[j]) == 0) { 冲 = 1; break; }
    }

    if (冲) {
        for (int i = 0; i < 数; i++) {
            int v = q->多[i];
            if (汇_态.寄[v] >= 0)
                fprintf(出, "    movq %s, %d(%%rbp)\n", 汇_寄名[汇_态.寄[v]], 汇_槽(v));
        }
        for (int i = 0; i < 数; i++)
            fprintf(出, "    movq %d(%%rbp), %s\n", 汇_槽(q->多[i]), 目[i]);
        return;
    }

    for (int i = 0; i < 数; i++) 汇_进(q->多[i], 目[i]);
}

/* 当前函数用到的 callee-saved，按压栈顺序排 */
static int 汇_稳序[汇_寄数];
static int 汇_稳总 = 0;

/* 尾声：把 callee-saved 从固定偏移读回来。偏移就是压栈时的位置 -8、-16… */
static void 汇_复位尾声(void)
{
    for (int i = 0; i < 汇_稳总; i++)
        fprintf(出, "    movq -%d(%%rbp), %s\n", 8 * (i + 1), 汇_寄名[汇_稳序[i]]);
}

static int 汇_是比较(int 码) { return 码 >= 指_等于 && 码 <= 指_大于等; }

static const char *汇_跳条件(int 码)
{
    switch (码) {
    case 指_等于:   return "je";
    case 指_不等:   return "jne";
    case 指_小于:   return "jl";
    case 指_大于:   return "jg";
    case 指_小于等: return "jle";
    case 指_大于等: return "jge";
    }
    return "jne";
}
static const char *汇_反跳条件(int 码)
{
    switch (码) {
    case 指_等于:   return "jne";
    case 指_不等:   return "je";
    case 指_小于:   return "jge";
    case 指_大于:   return "jle";
    case 指_小于等: return "jg";
    case 指_大于等: return "jl";
    }
    return "je";
}
static const char *汇_置条件(int 码)
{
    switch (码) {
    case 指_等于:   return "sete";
    case 指_不等:   return "setne";
    case 指_小于:   return "setl";
    case 指_大于:   return "setg";
    case 指_小于等: return "setle";
    case 指_大于等: return "setge";
    }
    return "sete";
}
static const char *汇_二位(int 码)
{
    switch (码) {
    case 指_加:   return "addq";
    case 指_减:   return "subq";
    case 指_乘:   return "imulq";
    case 指_位与: return "andq";
    case 指_位或: return "orq";
    case 指_位异或: return "xorq";
    }
    return "addq";
}

/* ---- 搬值：一个值（可能两个字）从 从 搬到 到 ---- */
static void 汇_搬值(中间函数 *f, int 从, int 到)
{
    if (从 < 0 || 到 < 0 || 从 == 到) return;
    int 两 = 汇_两字(值型(f, 从));
    int 从寄 = 汇_态.寄[从] >= 0, 到寄 = 汇_态.寄[到] >= 0;

    /* 常量直接写过去，没必要先落一个寄存器 */
    if (!从寄 && !两) {
        long long k;
        if (汇_速常(从, &k)) {
            if (到寄) fprintf(出, "    movq $%lld, %s\n", k, 汇_寄名[汇_态.寄[到]]);
            else fprintf(出, "    movq $%lld, %d(%%rbp)\n", k, 汇_槽(到));
            return;
        }
    }

    for (int 字 = 0; 字 < (两 ? 2 : 1); 字++) {
        int 偏 = 8 * 字;
        if (到寄) {
            const char *d = 汇_寄名[汇_态.寄[到]];
            if (从寄 && strcmp(汇_寄名[汇_态.寄[从]], d) == 0) continue;
            if (从寄) fprintf(出, "    movq %s, %s\n", 汇_寄名[汇_态.寄[从]], d);
            else fprintf(出, "    movq %d(%%rbp), %s\n", 汇_槽(从) + 偏, d);
        } else {
            if (从寄) fprintf(出, "    movq %s, %d(%%rbp)\n", 汇_寄名[汇_态.寄[从]], 汇_槽(到) + 偏);
            else {
                fprintf(出, "    movq %d(%%rbp), %%rax\n", 汇_槽(从) + 偏);
                fprintf(出, "    movq %%rax, %d(%%rbp)\n", 汇_槽(到) + 偏);
            }
        }
    }
}

static int 汇_序号(中间函数 *f);


/* ============================================================================
 *  除以常数的强度削减
 *  ----------------------------------------------------------------------------
 *  `idivq` 一条要二三十个周期，除法单元还是流水线的瓶颈。但除数是常数时，
 *  可以换成「乘一个倒数 + 移位」：编译器算好魔数 M 和位移 S，
 *
 *      x // d  ==  (x * M) >> (64 + S)          （M 是无符号 64 位，乘法取高 64 位）
 *
 *  这一步是编译器界的常识（Hacker's Delight 第十章），但魔数算错一个位，
 *  结果就会从某处开始整片地错，而且只在某些输入上错。所以这里不赌：
 *  算完之后拿一批样本（边界 + 随机）当场验一遍，验不过就老老实实用 idiv。
 *  验不过的概率为零，因为验不过就不生成这段代码。
 * ========================================================================== */

/* 有没有 64 位整数常量？ */
static int 汇_常整(int v, long long *出值)
{
    if (!汇_是常(v)) return 0;
    指令 *q = 汇_态.定义[v];
    if (!q || !q->文 || !q->型 || q->型->种 != 型_整数) return 0;
    char buf[64];
    int n = q->文长 < 63 ? q->文长 : 63;
    memcpy(buf, q->文, (size_t)n); buf[n] = 0;
    char *end = NULL;
    long long x = strtoll(buf, &end, 0);
    if (end == buf) return 0;
    *出值 = x;
    return 1;
}

/* x // d 的魔数；返回 0 表示「算不出来，用 idiv」。
 *
 * 思路：C 的整除是「向零截断」，所以先把 x 取绝对值，按无符号除，
 * 最后再把符号贴回去——这样只用一套无符号魔数，不用去啃有符号的
 * 那一堆分支。M = ceil(2^(64+s)/d)，s = ceil(log2 d)，乘法取高 64 位。
 */
static int 汇_幻数(long long d, unsigned long long *出M, int *出S)
{
    if (d < 2) return 0;
    if ((d & (d - 1)) == 0) {
        int k = 0;
        while ((1LL << k) < d) k++;
        /* 移位这条路也得验：负数补的是 d-1，不是 1，很容易写成补 1 */
        long long 试[7] = { 0, 1, -1, d, -d, 123456789, -123456789 };
        for (int i = 0; i < 7; i++) {
            long long x = 试[i];
            long long 掩 = (x < 0) ? (d - 1) : 0;
            if (((x + 掩) >> k) != x / d) return 0;
        }
        *出M = 0;                       /* 0 表示「走移位那条路」 */
        *出S = k;
        return 1;
    }
    /* s 要取到「2^s < d」，再多一位 M 就装不进 64 位了。
     * 这一位不是随便挑的：它保证 x*d <= 2^(64+s)，误差不会跨过整数边界。 */
    int s = 0;
    while (s < 62 && (1LL << (s + 1)) <= d) s++;
    __int128 分子 = ((__int128)1) << (64 + s);
    unsigned long long M = (unsigned long long)(分子 / d + 1);
    if ((__int128)M != 分子 / d + 1) return 0;      /* 装不下 64 位就算了 */

    long long 样[28];
    int 样数 = 0;
    样[样数++] = 0; 样[样数++] = 1; 样[样数++] = -1;
    样[样数++] = d; 样[样数++] = -d; 样[样数++] = d + 1; 样[样数++] = -d - 1;
    样[样数++] = d - 1; 样[样数++] = -(d - 1);
    样[样数++] = 0x7fffffffffffffffLL; 样[样数++] = -0x7fffffffffffffffLL;
    样[样数++] = 0x4000000000000000LL; 样[样数++] = -0x4000000000000000LL;
    样[样数++] = 0x4000000000000001LL; 样[样数++] = -0x4000000000000001LL;
    unsigned long long 种 = 88172645463325252ULL;
    for (int i = 0; i < 13; i++) {
        种 ^= 种 << 13; 种 ^= 种 >> 7; 种 ^= 种 << 17;
        样[样数++] = (long long)种 >> (i & 7);
    }
    for (int i = 0; i < 样数; i++) {
        long long x = 样[i];
        long long 应 = x / d;
        unsigned long long ax = (unsigned long long)(x < 0 ? -x : x);
        unsigned long long 得 = (unsigned long long)(((__int128)ax * M) >> (64 + s));
        long long q = (x < 0) ? -(long long)得 : (long long)得;
        if (应 != q) return 0;
    }
    *出M = M; *出S = s;
    return 1;
}

/* ---- 一条指令 ---- */
/* ---- 浮点 ----
 * 浮点值不参与通用寄存器分配（寄[v] 保持 -1），一律躺在家槽里。
 * 一条浮点指令就是「从槽读、算、写回槽」：多两次访存，换来的是不用再写
 * 一套 xmm 的活跃分析与溢出策略。先把浮点算对，速度是下一步的事。
 *
 * 也不碰 xmm 以外的约定：实参还是走 rdi/rsi...，浮点形参暂时不收。 */
static int 汇_是浮(中间函数 *f, int v)
{
    类型 *t = 值型(f, v);
    return t && t->种 == 型_浮点;
}

/* 整数结果要落到分配器指定的地方：有寄存器就写寄存器，否则写家槽。
 * 浮点比较、浮点转整数都会产生整数——只写槽的话，后面按寄存器读就是垃圾。 */
static void 汇_落整数(int v, const char *r)
{
    if (v >= 0 && v < 汇_态.值数 && 汇_态.寄[v] >= 0)
        fprintf(出, "    movq %s, %s\n", r, 汇_寄名[汇_态.寄[v]]);
    else
        fprintf(出, "    movq %s, %d(%%rbp)\n", r, 汇_槽(v));
}

static int 汇_浮点单条(中间函数 *f, 指令 *q)
{
    int 果 = q->结果, 甲 = q->甲, 乙 = q->乙;

    switch (q->码) {
    case 指_常量:
        if (q->型 && q->型->种 == 型_浮点) {
            int 号 = 汇_浮号(汇_浮文(q->文, q->文长));
            fprintf(出, "    movsd .LCF%d(%%rip), %%xmm0\n", 号);
            fprintf(出, "    movsd %%xmm0, %d(%%rbp)\n", 汇_槽(果));
            return 1;
        }
        return 0;

    case 指_加: case 指_减: case 指_乘: case 指_除: {
        if (!汇_是浮(f, 甲) && !汇_是浮(f, 乙)) return 0;
        const char *op = q->码 == 指_加 ? "addsd" : q->码 == 指_减 ? "subsd"
                       : q->码 == 指_乘 ? "mulsd" : "divsd";
        fprintf(出, "    movsd %d(%%rbp), %%xmm0\n", 汇_槽(甲));
        fprintf(出, "    %s %d(%%rbp), %%xmm0\n", op, 汇_槽(乙));
        fprintf(出, "    movsd %%xmm0, %d(%%rbp)\n", 汇_槽(果));
        return 1;
    }

    case 指_等于: case 指_不等: case 指_小于: case 指_大于:
    case 指_小于等: case 指_大于等: {
        if (!汇_是浮(f, 甲) && !汇_是浮(f, 乙)) return 0;
        /* ucomisd 的 CF 表示「第一个操作数小于第二个」：无序数（NaN）下 CF=1，
         * 所以 <、<= 对 NaN 会给真——和 C 的 < 不一样。这一版不处理 NaN。 */
        const char *cc = q->码 == 指_等于 ? "sete" : q->码 == 指_不等 ? "setne"
                       : q->码 == 指_小于 ? "setb" : q->码 == 指_大于 ? "seta"
                       : q->码 == 指_小于等 ? "setbe" : "setae";
        fprintf(出, "    movsd %d(%%rbp), %%xmm0\n", 汇_槽(甲));
        fprintf(出, "    ucomisd %d(%%rbp), %%xmm0\n", 汇_槽(乙));
        fprintf(出, "    %s %%al\n", cc);
        fputs("    movzbq %al, %rax\n", 出);
        汇_落整数(果, "%rax");
        return 1;
    }

    case 指_负:
        if (!汇_是浮(f, 甲)) return 0;
        /* 是「翻转」不是「清掉」：btr 会得到绝对值，-1.5 还是 1.5。 */
        fprintf(出, "    movq %d(%%rbp), %%rax\n", 汇_槽(甲));
        fputs("    btcq $63, %rax\n", 出);
        fprintf(出, "    movq %%rax, %d(%%rbp)\n", 汇_槽(果));
        return 1;

    case 指_转换: {
        类型 *到 = q->型, *从 = 值型(f, 甲);
        if (!到 || !从) return 0;
        if (到->种 == 型_浮点 && 从->种 == 型_浮点) { 汇_搬值(f, 甲, 果); return 1; }
        if (到->种 == 型_浮点 && (从->种 == 型_整数 || 从->种 == 型_布尔)) {
            /* 整数常量不落槽，得让它先变成一个真实的寄存器 */
            const char *r = 汇_取(甲);
            fprintf(出, "    cvtsi2sd %s, %%xmm0\n", r);
            fprintf(出, "    movsd %%xmm0, %d(%%rbp)\n", 汇_槽(果));
            return 1;
        }
        if (到->种 == 型_整数 && 从->种 == 型_浮点) {
            fprintf(出, "    cvttsd2si %d(%%rbp), %%rax\n", 汇_槽(甲));
            汇_落整数(果, "%rax");
            return 1;
        }
        return 0;
    }

    default:
        return 0;
    }
}

static void 汇_单条(中间函数 *f, int 序, 指令 *q, int idx)
{
    int 果 = q->结果, 甲 = q->甲, 乙 = q->乙;

    /* 结果没人用的纯指令，整条不要 */
    if (果 >= 0 && 果 < 汇_态.值数 && 汇_态.用次[果] == 0 && 汇_纯(q->码)) return;

    汇_池清();

    /* 浮点先走一边：它只碰家槽和 xmm，不该跟整数路径混在一起 */
    if (汇_浮点单条(f, q)) return;

    switch (q->码) {

    case 指_空操作:
    case 指_丢弃:
        return;

    case 指_常量: {
        类型 *t = q->型;
        if (t && t->种 == 型_字符串) {
            int 号 = 汇_串号(q->文, q->文长);
            if (汇_态.寄[果] >= 0) {
                fprintf(出, "    leaq .LSTR%d(%%rip), %s\n", 号, 汇_寄名[汇_态.寄[果]]);
                fprintf(出, "    movq $%d, %d(%%rbp)\n", 汇_串[号].长, 汇_槽(果) + 8);
            } else {
                fprintf(出, "    leaq .LSTR%d(%%rip), %%rax\n", 号);
                fprintf(出, "    movq %%rax, %d(%%rbp)\n", 汇_槽(果));
                fprintf(出, "    movq $%d, %d(%%rbp)\n", 汇_串[号].长, 汇_槽(果) + 8);
            }
            return;
        }
        if (t && t->种 == 型_布尔) {
            int 真 = (q->文长 == 3 && q->文 && memcmp(q->文, "真", 3) == 0);
            if (汇_态.寄[果] >= 0) fprintf(出, "    movq $%d, %s\n", 真, 汇_寄名[汇_态.寄[果]]);
            return;
        }
        if (t && t->种 == 型_整数) {
            /* 整数常量不落寄存器也不落栈：用到它的地方直接写立即数 */
            if (汇_态.寄[果] >= 0)
                fprintf(出, "    movq $%.*s, %s\n", q->文长, q->文, 汇_寄名[汇_态.寄[果]]);
            return;
        }
        汇_坏(f, q, "这种常量类型");
        return;
    }

    case 指_载: case 指_移动: case 指_复制: case 指_借用: case 指_可变借用:
        汇_搬值(f, 甲, 果);
        return;

    case 指_存:
        汇_搬值(f, 乙, 甲);
        return;

    /* ---- 整数二元 ---- */
    case 指_加: case 指_减: case 指_乘:
    case 指_位与: case 指_位或: case 指_位异或: {
        long long k;
        const char *a = 汇_取(甲);
        int 落; const char *d = 汇_目(果, &落);
        int 可交换 = (q->码 != 指_减);

        if (汇_小常(乙, &k)) {
            if (q->码 == 指_加) {
                if (strcmp(a, d) == 0) fprintf(出, "    addq $%lld, %s\n", k, d);
                else fprintf(出, "    leaq %lld(%s), %s\n", k, a, d);
            } else if (q->码 == 指_减) {
                if (strcmp(a, d) == 0) fprintf(出, "    subq $%lld, %s\n", k, d);
                else fprintf(出, "    leaq -%lld(%s), %s\n", k, a, d);
            } else if (q->码 == 指_乘) {
                if (strcmp(a, d) == 0) fprintf(出, "    imulq $%lld, %s\n", k, d);
                else fprintf(出, "    imulq $%lld, %s, %s\n", k, a, d);
            } else {
                const char *op = 汇_二位(q->码);
                if (strcmp(a, d) != 0) fprintf(出, "    movq %s, %s\n", a, d);
                fprintf(出, "    %s $%lld, %s\n", op, k, d);
            }
            汇_落值(果, 落, d);
            return;
        }

        const char *b = 汇_取(乙);

        if (q->码 == 指_加) {
            if (strcmp(a, d) == 0) fprintf(出, "    addq %s, %s\n", b, d);
            else if (strcmp(b, d) == 0) fprintf(出, "    addq %s, %s\n", a, d);
            else fprintf(出, "    leaq (%s,%s), %s\n", a, b, d);
        } else if (q->码 == 指_减) {
            if (strcmp(a, d) == 0) fprintf(出, "    subq %s, %s\n", b, d);
            else if (strcmp(b, d) == 0) {
                const char *t = 汇_池拿();
                fprintf(出, "    movq %s, %s\n", a, t);
                fprintf(出, "    subq %s, %s\n", b, t);
                fprintf(出, "    movq %s, %s\n", t, d);
            } else {
                fprintf(出, "    movq %s, %s\n", a, d);
                fprintf(出, "    subq %s, %s\n", b, d);
            }
        } else {
            const char *op = 汇_二位(q->码);
            if (strcmp(a, d) == 0) fprintf(出, "    %s %s, %s\n", op, b, d);
            else if (可交换 && strcmp(b, d) == 0) fprintf(出, "    %s %s, %s\n", op, a, d);
            else {
                fprintf(出, "    movq %s, %s\n", a, d);
                fprintf(出, "    %s %s, %s\n", op, b, d);
            }
        }
        汇_落值(果, 落, d);
        return;
    }

    case 指_左移: case 指_右移: {
        long long k;
        const char *a = 汇_取(甲);
        int 落; const char *d = 汇_目(果, &落);
        const char *op = (q->码 == 指_左移) ? "shlq" : "sarq";

        if (汇_小常(乙, &k) && k >= 0 && k <= 63) {
            if (strcmp(a, d) != 0) fprintf(出, "    movq %s, %s\n", a, d);
            fprintf(出, "    %s $%lld, %s\n", op, k, d);
        } else {
            /* 移位量必须在 %cl，先把 %rcx 锁下来，再取甲，两边就不打架 */
            const char *b = 汇_取(乙);
            if (strcmp(b, "%rcx") != 0) {
                fprintf(出, "    movq %s, %%rcx\n", b);
                if (strcmp(b, "%rax") == 0 || strcmp(b, "%rdx") == 0) 汇_池放(b);
            }
            汇_池占[1] = 1;                       /* %rcx 已被占用 */
            if (strcmp(a, "%rcx") == 0) {
                const char *t = 汇_池拿();
                fprintf(出, "    movq %s, %s\n", a, t);
                a = t;
            }
            int 落2; const char *d2 = 汇_目(果, &落2);
            (void)d;
            if (strcmp(a, d2) != 0) fprintf(出, "    movq %s, %s\n", a, d2);
            fprintf(出, "    %s %%cl, %s\n", op, d2);
            汇_落值(果, 落2, d2);
            return;
        }
        汇_落值(果, 落, d);
        return;
    }

    case 指_整除: case 指_取余: {
        long long d = 0;
        unsigned long long M = 0;
        int S = 0;
        int 有魔 = 汇_常整(乙, &d) && d >= 2 && 汇_幻数(d, &M, &S);

        if (有魔) {
            /* 甲的来源有三种：立即数常量、分配到的寄存器、家槽。
             * 常量那条不能漏——整数常量不落槽，槽里是上一轮函数留下的垃圾。 */
            int 甲寄 = (甲 >= 0 && 汇_态.寄[甲] >= 0);
            long long 甲常值 = 0;
            int 甲常 = 汇_速常(甲, &甲常值);
            if (甲常) {
                fprintf(出, "    movq $%lld, %%rax\n", 甲常值);
            } else if (甲寄) {
                const char *a = 汇_寄名[汇_态.寄[甲]];
                if (strcmp(a, "%rax") != 0) fprintf(出, "    movq %s, %%rax\n", a);
            } else {
                fprintf(出, "    movq %d(%%rbp), %%rax\n", 汇_槽(甲));
            }

            if (M == 0) {
                /* 2 的幂：q = (x + ((x >> 63) & (d-1))) >> k
                 * 注意是算术右移拿全 1 掩码——负数要补 d-1，不是补 1。 */
                fputs("    movq %rax, %rcx\n    sarq $63, %rcx\n", 出);
                fprintf(出, "    andq $%lld, %%rcx\n", d - 1);
                fputs("    addq %rcx, %rax\n", 出);
                fprintf(出, "    sarq $%d, %%rax\n", S);
            } else {
                /* 取绝对值 → 无符号乘高 → 移回符号 */
                fputs("    movq %rax, %rcx\n    sarq $63, %rcx\n", 出);
                fputs("    xorq %rcx, %rax\n    subq %rcx, %rax\n", 出);
                fprintf(出, "    movabsq $%llu, %%rdx\n", M);
                fputs("    mulq %rdx\n", 出);                 /* rdx:rax = |x| * M */
                fprintf(出, "    shrq $%d, %%rdx\n", S);
                fputs("    movq %rdx, %rax\n    xorq %rcx, %rax\n    subq %rcx, %rax\n", 出);
            }

            if (q->码 == 指_整除) {
                if (汇_态.寄[果] >= 0) fprintf(出, "    movq %%rax, %s\n", 汇_寄名[汇_态.寄[果]]);
                else fprintf(出, "    movq %%rax, %d(%%rbp)\n", 汇_槽(果));
            } else {
                /* r = x - q*d */
                fputs("    movq %rax, %rdx\n", 出);
                if (d >= -2147483648LL && d <= 2147483647LL) {
                    fprintf(出, "    imulq $%lld, %%rdx\n", d);
                } else {
                    fprintf(出, "    movabsq $%lld, %%rcx\n", d);
                    fputs("    imulq %rcx, %rdx\n", 出);
                }
                if (甲常) fprintf(出, "    movq $%lld, %%rax\n", 甲常值);
                else if (甲寄) fprintf(出, "    movq %s, %%rax\n", 汇_寄名[汇_态.寄[甲]]);
                else fprintf(出, "    movq %d(%%rbp), %%rax\n", 汇_槽(甲));
                fputs("    subq %rdx, %rax\n", 出);
                if (汇_态.寄[果] >= 0) fprintf(出, "    movq %%rax, %s\n", 汇_寄名[汇_态.寄[果]]);
                else fprintf(出, "    movq %%rax, %d(%%rbp)\n", 汇_槽(果));
            }
            return;
        }

        const char *a = 汇_取(甲);
        const char *b = 汇_取(乙);
        if (strcmp(b, "%rax") == 0 || strcmp(b, "%rdx") == 0) {
            const char *t = 汇_池拿();
            fprintf(出, "    movq %s, %s\n", b, t);
            汇_池放(b);
            b = t;
        }
        if (strcmp(a, "%rax") != 0) fprintf(出, "    movq %s, %%rax\n", a);
        fputs("    cqto\n", 出);
        fprintf(出, "    idivq %s\n", b);
        if (q->码 == 指_取余) {
            if (汇_态.寄[果] >= 0) fprintf(出, "    movq %%rdx, %s\n", 汇_寄名[汇_态.寄[果]]);
            else fprintf(出, "    movq %%rdx, %d(%%rbp)\n", 汇_槽(果));
        } else {
            if (汇_态.寄[果] >= 0) fprintf(出, "    movq %%rax, %s\n", 汇_寄名[汇_态.寄[果]]);
            else fprintf(出, "    movq %%rax, %d(%%rbp)\n", 汇_槽(果));
        }
        return;
    }

    case 指_除:
        汇_坏(f, q, "/（浮点除法）—— 用 // 做整除，浮点还没接");
        return;

    case 指_乘方: {
        static const char *目[2] = { "%rdi", "%rsi" };
        汇_存跨(idx);
        汇_传参(q, 目, 2);
        fputs("    call 简_幂\n", 出);
        汇_取跨(idx);
        if (果 >= 0) {
            if (汇_态.寄[果] >= 0) fprintf(出, "    movq %%rax, %s\n", 汇_寄名[汇_态.寄[果]]);
            else fprintf(出, "    movq %%rax, %d(%%rbp)\n", 汇_槽(果));
        }
        return;
    }

    /* ---- 比较 ---- */
    case 指_等于: case 指_不等:
    case 指_小于: case 指_大于: case 指_小于等: case 指_大于等: {
        /* 后面紧跟着跳它，就别算 0/1，直接把比较留给那条跳 */
        if (q->下 && q->下->码 == 指_条件跳 && q->下->甲 == 果 &&
            果 >= 0 && 果 < 汇_态.值数 && 汇_态.用次[果] == 1)
            return;

        long long k;
        const char *a = 汇_取(甲);
        if (汇_小常(乙, &k)) fprintf(出, "    cmpq $%lld, %s\n", k, a);
        else {
            const char *b = 汇_取(乙);
            fprintf(出, "    cmpq %s, %s\n", b, a);
        }
        fprintf(出, "    %s %%al\n", 汇_置条件(q->码));
        fputs("    movzbq %al, %rax\n", 出);
        if (汇_态.寄[果] >= 0) {
            const char *d = 汇_寄名[汇_态.寄[果]];
            if (strcmp(d, "%rax") != 0) fprintf(出, "    movq %%rax, %s\n", d);
        } else {
            fprintf(出, "    movq %%rax, %d(%%rbp)\n", 汇_槽(果));
        }
        return;
    }

    case 指_非: {
        const char *a = 汇_取(甲);
        fprintf(出, "    testq %s, %s\n", a, a);
        fputs("    sete %al\n    movzbq %al, %rax\n", 出);
        if (汇_态.寄[果] >= 0) {
            const char *d = 汇_寄名[汇_态.寄[果]];
            if (strcmp(d, "%rax") != 0) fprintf(出, "    movq %%rax, %s\n", d);
        } else fprintf(出, "    movq %%rax, %d(%%rbp)\n", 汇_槽(果));
        return;
    }

    case 指_负: case 指_取反: {
        const char *a = 汇_取(甲);
        int 落; const char *d = 汇_目(果, &落);
        if (strcmp(a, d) != 0) fprintf(出, "    movq %s, %s\n", a, d);
        fprintf(出, "    %s %s\n", q->码 == 指_负 ? "negq" : "notq", d);
        汇_落值(果, 落, d);
        return;
    }

    case 指_长度: {
        /* 字符串的 # 是字符数（UTF-8 一个汉字算一个），不是字节数。
         * 组的 # 是元素个数、映射的 # 是键数，那些照旧读第二个字。 */
        类型 *t = 值型(f, 甲);
        if (t && t->种 == 型_字符串) {
            /* 调一次运行时，caller-saved 里的活值得先收起来 */
            汇_存跨(idx);
            if (汇_态.寄[甲] >= 0)
                fprintf(出, "    movq %s, %d(%%rbp)\n", 汇_寄名[汇_态.寄[甲]], 汇_槽(甲));
            fprintf(出, "    movq %d(%%rbp), %%rdi\n", 汇_槽(甲));
            fprintf(出, "    movq %d(%%rbp), %%rsi\n", 汇_槽(甲) + 8);
            fputs("    call 简_字数\n", 出);
            汇_取跨(idx);
            汇_落整数(果, "%rax");
            return;
        }
        if (汇_态.寄[果] >= 0)
            fprintf(出, "    movq %d(%%rbp), %s\n", 汇_槽(甲) + 8, 汇_寄名[汇_态.寄[果]]);
        else {
            fprintf(出, "    movq %d(%%rbp), %%rax\n", 汇_槽(甲) + 8);
            fprintf(出, "    movq %%rax, %d(%%rbp)\n", 汇_槽(果));
        }
        return;
    }

    case 指_拼接: {
        汇_存跨(idx);
        if (汇_态.寄[甲] >= 0)
            fprintf(出, "    movq %s, %d(%%rbp)\n", 汇_寄名[汇_态.寄[甲]], 汇_槽(甲));
        if (汇_态.寄[乙] >= 0)
            fprintf(出, "    movq %s, %d(%%rbp)\n", 汇_寄名[汇_态.寄[乙]], 汇_槽(乙));
        fprintf(出, "    movq %d(%%rbp), %%rdi\n", 汇_槽(甲));
        fprintf(出, "    movq %d(%%rbp), %%rsi\n", 汇_槽(甲) + 8);
        fprintf(出, "    movq %d(%%rbp), %%rdx\n", 汇_槽(乙));
        fprintf(出, "    movq %d(%%rbp), %%rcx\n", 汇_槽(乙) + 8);
        fputs("    call 简_拼\n", 出);
        汇_取跨(idx);
        if (汇_态.寄[果] >= 0) fprintf(出, "    movq %%rax, %s\n", 汇_寄名[汇_态.寄[果]]);
        else fprintf(出, "    movq %%rax, %d(%%rbp)\n", 汇_槽(果));
        fprintf(出, "    movq %%rdx, %d(%%rbp)\n", 汇_槽(果) + 8);
        return;
    }

    /* ---- 控制流 ---- */
    case 指_跳:
        /* 目标就是物理上的下一块 → 直接掉下去，不用跳 */
        if (汇_下块 && 汇_态.号块 && 甲 >= 0 && 汇_态.号块[甲] == 汇_下块) return;
        fprintf(出, "    jmp .L%d_%d\n", 序, 甲);
        return;

    case 指_条件跳: {
        int v = 甲;
        long long k;

        if (v >= 0 && v < 汇_态.值数) {
            指令 *cmp = 汇_态.定义[v];
            if (cmp && cmp->下 == q && 汇_是比较(cmp->码) && 汇_态.用次[v] == 1) {
                const char *a = 汇_取(cmp->甲);
                if (汇_小常(cmp->乙, &k)) fprintf(出, "    cmpq $%lld, %s\n", k, a);
                else {
                    const char *b = 汇_取(cmp->乙);
                    fprintf(出, "    cmpq %s, %s\n", b, a);
                }
                if (汇_下块 && 汇_态.号块 && q->丙 >= 0 && 汇_态.号块[q->丙] == 汇_下块) {
                    /* 假分支就在下面：条件成立时跳走，否则直接掉进假分支 */
                    fprintf(出, "    %s .L%d_%d\n", 汇_跳条件(cmp->码), 序, 乙);
                } else if (汇_下块 && 汇_态.号块 && 乙 >= 0 && 汇_态.号块[乙] == 汇_下块) {
                    fprintf(出, "    %s .L%d_%d\n", 汇_反跳条件(cmp->码), 序, q->丙);
                } else {
                    fprintf(出, "    %s .L%d_%d\n", 汇_跳条件(cmp->码), 序, 乙);
                    fprintf(出, "    jmp .L%d_%d\n", 序, q->丙);
                }
                return;
            }
        }

        if (汇_立即(v, &k)) {
            fprintf(出, "    jmp .L%d_%d\n", 序, k != 0 ? 乙 : q->丙);
            return;
        }

        const char *a = 汇_取(v);
        fprintf(出, "    testq %s, %s\n", a, a);
        if (汇_下块 && 汇_态.号块 && q->丙 >= 0 && 汇_态.号块[q->丙] == 汇_下块) {
            fprintf(出, "    jne .L%d_%d\n", 序, 乙);
        } else if (汇_下块 && 汇_态.号块 && 乙 >= 0 && 汇_态.号块[乙] == 汇_下块) {
            fprintf(出, "    je .L%d_%d\n", 序, q->丙);
        } else {
            fprintf(出, "    jne .L%d_%d\n", 序, 乙);
            fprintf(出, "    jmp .L%d_%d\n", 序, q->丙);
        }
        return;
    }

    case 指_返回: {
        if (甲 >= 0) {
            汇_进(甲, "%rax");
            if (汇_两字(值型(f, 甲)))
                fprintf(出, "    movq %d(%%rbp), %%rdx\n", 汇_槽(甲) + 8);
        } else {
            fputs("    xorl %eax, %eax\n", 出);
        }
        汇_复位尾声();
        fputs("    leave\n    ret\n", 出);
        return;
    }

    case 指_转换: {
        类型 *到 = q->型;
        类型 *从 = 值型(f, 甲);
        if (到 && (到->种 == 型_整数 || 到->种 == 型_布尔) &&
            从 && (从->种 == 型_整数 || 从->种 == 型_布尔)) {
            汇_搬值(f, 甲, 果);
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
                    汇_存跨(idx);
                    汇_进(v, "%rdi");
                    fprintf(出, "    movq %d(%%rbp), %%rsi\n", 汇_槽(v) + 8);
                    fputs("    call 简_写\n", 出);
                    汇_取跨(idx);
                } else if (t && t->种 == 型_浮点) {
                    汇_存跨(idx);
                    fprintf(出, "    movsd %d(%%rbp), %%xmm0\n", 汇_槽(v));
                    fputs("    call 简_打浮\n", 出);
                    汇_取跨(idx);
                } else if (t && t->种 == 型_布尔) {
                    long long kk;
                    int 是常 = 汇_速常(v, &kk);
                    if (是常) {
                        汇_存跨(idx);
                        fputs(kk ? "    leaq .LTRUE(%rip), %rdi\n" : "    leaq .LFALSE(%rip), %rdi\n", 出);
                        fputs("    movq $3, %rsi\n    call 简_写\n", 出);
                        汇_取跨(idx);
                        continue;
                    }
                    汇_存跨(idx);
                    int 号 = 汇_标号++;
                    {
                        const char *r = 汇_取(v);
                        fprintf(出, "    testq %s, %s\n", r, r);
                    }
                    fprintf(出, "    je .Lbool%d_f\n", 号);
                    fputs("    leaq .LTRUE(%rip), %rdi\n    movq $3, %rsi\n    call 简_写\n", 出);
                    fprintf(出, "    jmp .Lbool%d_e\n.Lbool%d_f:\n", 号, 号);
                    fputs("    leaq .LFALSE(%rip), %rdi\n    movq $3, %rsi\n    call 简_写\n", 出);
                    fprintf(出, ".Lbool%d_e:\n", 号);
                    汇_取跨(idx);
                } else if (t && t->种 == 型_整数) {
                    汇_存跨(idx);
                    汇_进(v, "%rdi");
                    fputs("    call 简_打整\n", 出);
                    汇_取跨(idx);
                } else {
                    汇_坏(f, q, "打印这种类型");
                    return;
                }
            }
            if (打印行) {
                汇_存跨(idx);
                fputs("    call 简_换行\n", 出);
                汇_取跨(idx);
            }
            return;
        }

        中间函数 *g = 找函数(q->文, q->文长);
        if (!g) { 汇_坏(f, q, "调用了未知函数"); return; }
        if (q->多量 > 6) { 汇_坏(f, q, "实参超过 6 个"); return; }
        for (int i = 0; i < q->多量; i++) {
            类型 *t = 值型(f, q->多[i]);
            if (t && (t->种 == 型_字符串 || t->种 == 型_浮点 || t->种 == 型_组 ||
                      t->种 == 型_映射 || t->种 == 型_命名 || t->种 == 型_声音)) {
                汇_坏(f, q, "这类类型的实参");
                return;
            }
        }
        int 序g = 汇_序号(g);
        if (序g < 0) { 汇_坏(f, q, "调用了没编译的函数"); return; }

        static const char *目[6] = { "%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9" };
        汇_存跨(idx);
        汇_传参(q, 目, q->多量);
        fprintf(出, "    call 汇F%d\n", 序g);
        汇_取跨(idx);
        if (果 >= 0) {
            if (汇_态.寄[果] >= 0) fprintf(出, "    movq %%rax, %s\n", 汇_寄名[汇_态.寄[果]]);
            else fprintf(出, "    movq %%rax, %d(%%rbp)\n", 汇_槽(果));
        }
        return;
    }

    default:
        汇_坏(f, q, "这条指令");
        return;
    }
}

/* ============================================================================
 *  第四部分：把 载/存 就地化简
 *  ----------------------------------------------------------------------------
 *  IR 里每个变量都是一个「槽」，读它先 `v = 载 槽`，写它 `槽 = 存 v`。
 *  照直生成就是每条读写各一条 mov——`和 = 和 + i` 会变成四条搬运加一条加法。
 *
 *  但这两个 mov 几乎都是白搬的：
 *
 *    - `v = 载 x`，v 只被用一次，就在下一条——把那次使用直接改成用 x。
 *    - `v = 算...`，v 只被 `x = 存 v` 用一次——让那条算式直接写进 x。
 *
 *  化简完 `和 = 和 + i` 就是一条 `addq`。这不是「优化技巧」，只是把
 *  IR 里为了统一格式而多出来的搬运去掉了。
 *
 *  只在同一条基本块里做：跨块要证明「中间没人改这个变量」，那是另一件事。
 * ========================================================================== */
static int 汇_同值(类型 *a, 类型 *b)
{
    if (!a || !b) return a == b;
    return a->种 == b->种;
}

/* 把指令 q 的操作数 from 换成 to（甲/乙/丙 和多参数表） */
static void 汇_换用(指令 *q, int from, int to)
{
    for (int i = 0; i < 3; i++) {
        int v = (i == 0) ? q->甲 : (i == 1) ? q->乙 : q->丙;
        if (v != from) continue;
        if (i == 0 && q->码 == 指_跳) continue;
        if (i > 0 && q->码 == 指_条件跳) continue;
        if (i == 0) q->甲 = to;
        else if (i == 1) q->乙 = to;
        else q->丙 = to;
    }
    for (int i = 0; i < q->多量; i++) if (q->多[i] == from) q->多[i] = to;
}

/* q 之后、到 NULL 之前，有没有用到 v；顺带看有没有 存 v */
static int 汇_中间用(int v, 指令 *起, 指令 *止, 指令 **命中)
{
    for (指令 *q = 起; q && q != 止; q = q->下) {
        for (int i = 0; i < 3; i++) if (汇_操作数(q, i) == v) { if (命中) *命中 = q; return 1; }
        for (int i = 0; i < q->多量; i++) if (q->多[i] == v) { if (命中) *命中 = q; return 1; }
    }
    return 0;
}

static int 汇_中间存(int v, 指令 *起, 指令 *止)
{
    for (指令 *q = 起; q && q != 止; q = q->下)
        if (q->码 == 指_存 && q->甲 == v) return 1;
    return 0;
}

static void 汇_简化(中间函数 *f)
{
    int 值数 = f->值数;
    if (值数 <= 0) return;
    int *用次 = (int *)calloc((size_t)值数, sizeof(int));
    指令 **用指 = (指令 **)calloc((size_t)值数, sizeof(指令 *));
    指令 **定义 = (指令 **)calloc((size_t)值数, sizeof(指令 *));

    for (int 遍 = 0; 遍 < 2; 遍++) {
        memset(用次, 0, sizeof(int) * (size_t)值数);
        memset(用指, 0, sizeof(指令 *) * (size_t)值数);
        memset(定义, 0, sizeof(指令 *) * (size_t)值数);
        for (基本块 *b = f->首块; b; b = b->下)
            for (指令 *q = b->头; q; q = q->下) {
                for (int i = 0; i < 3; i++) {
                    int v = 汇_操作数(q, i);
                    if (v >= 0 && v < 值数) { 用次[v]++; 用指[v] = q; }
                }
                for (int i = 0; i < q->多量; i++) {
                    int v = q->多[i];
                    if (v >= 0 && v < 值数) { 用次[v]++; 用指[v] = q; }
                }
                if (q->结果 >= 0 && q->结果 < 值数) 定义[q->结果] = q;
            }

        if (遍 == 0) {
            /* `v = 载 x`，v 只有这一个用处 → 那个用处直接用 x */
            for (基本块 *b = f->首块; b; b = b->下)
                for (指令 *q = b->头; q; q = q->下) {
                    if (q->码 != 指_载 && q->码 != 指_移动 && q->码 != 指_复制) continue;
                    int v = q->结果, x = q->甲;
                    if (v < 0 || x < 0 || v >= 值数 || x >= 值数) continue;
                    if (用次[v] != 1) continue;
                    指令 *用 = 用指[v];
                    if (!用) continue;
                    if (!汇_同值(值型(f, v), 值型(f, x))) continue;
                    if (汇_中间存(x, q->下, 用)) continue;
                    /* 用 必须在同一块的后续里 */
                    int 找 = 0;
                    for (指令 *t = q->下; t; t = t->下) if (t == 用) { 找 = 1; break; }
                    if (!找) continue;
                    汇_换用(用, v, x);
                    q->码 = 指_空操作;
                    q->结果 = -1;
                }
        } else {
            /* `x = 存 v`，v 只有这一个用处 → 让算 v 的那条直接写进 x */
            for (基本块 *b = f->首块; b; b = b->下)
                for (指令 *q = b->头; q; q = q->下) {
                    if (q->码 != 指_存) continue;
                    int x = q->甲, v = q->乙;
                    if (v < 0 || x < 0 || v >= 值数 || x >= 值数) continue;
                    if (用次[v] != 1) continue;
                    指令 *d = 定义[v];
                    if (!d || d->结果 != v) continue;
                    switch (d->码) {
                    case 指_载: case 指_移动: case 指_复制: case 指_存:
                    case 指_调用: case 指_调用方法: case 指_拼接:
                        continue;
                    default: break;
                    }
                    if (!汇_同值(值型(f, v), 值型(f, x))) continue;
                    /* d 必须在同一块、且在 q 前面 */
                    int 找 = 0;
                    for (指令 *t = d->下; t; t = t->下) if (t == q) { 找 = 1; break; }
                    if (!找) continue;
                    /* d 到 q 之间不能有人读 x，否则会提前看到新值 */
                    if (汇_中间用(x, d->下, q, NULL)) continue;
                    d->结果 = x;
                    q->码 = 指_空操作;
                }
        }
    }

    free(用次); free(用指); free(定义);
}

/* ---- 一个函数 ---- */
static void 写函数汇编(中间函数 *f, int 序)
{
    memset(&汇_态, 0, sizeof(汇_态));
    汇_态.f = f;
    汇_态.值数 = f->值数;

    int 值数 = f->值数;
    汇_态.槽 = (int *)calloc((size_t)(值数 + 1), sizeof(int));
    汇_态.寄 = (int *)malloc(sizeof(int) * (size_t)(值数 + 1));
    汇_态.用次 = (int *)calloc((size_t)(值数 + 1), sizeof(int));
    汇_态.定义 = (指令 **)calloc((size_t)(值数 + 1), sizeof(指令 *));
    汇_态.常值 = (unsigned char *)calloc((size_t)(值数 + 1), 1);
    int *定数 = (int *)calloc((size_t)(值数 + 1), sizeof(int));
    for (int v = 0; v <= 值数; v++) 汇_态.寄[v] = -1;

    /* 块表（发射顺序） */
    int 块数 = 0;
    for (基本块 *b = f->首块; b; b = b->下) 块数++;
    汇_态.块数 = 块数;
    汇_态.块 = (基本块 **)calloc((size_t)(块数 + 1), sizeof(基本块 *));
    汇_态.块起 = (int *)calloc((size_t)(块数 + 1), sizeof(int));
    汇_态.块条 = (int *)calloc((size_t)(块数 + 1), sizeof(int));
    int k = 0;
    for (基本块 *b = f->首块; b; b = b->下) 汇_态.块[k++] = b;

    /* 数指令 + 用次 + 定义 */
    int 总 = 0;
    for (int b = 0; b < 块数; b++) {
        汇_态.块起[b] = 总;
        int 条 = 0;
        for (指令 *q = 汇_态.块[b]->头; q; q = q->下) {
            for (int i = 0; i < 3; i++) {
                int v = 汇_操作数(q, i);
                if (v >= 0 && v < 值数) 汇_态.用次[v]++;
            }
            for (int i = 0; i < q->多量; i++) {
                int v = q->多[i];
                if (v >= 0 && v < 值数) 汇_态.用次[v]++;
            }
            if (q->结果 >= 0 && q->结果 < 值数) { 汇_态.定义[q->结果] = q; 定数[q->结果]++; }
            if (q->码 == 指_存 && q->甲 >= 0 && q->甲 < 值数) 定数[q->甲]++;
            条++; 总++;
        }
        汇_态.块条[b] = 条;
    }
    汇_态.指令总 = 总;

    /* 「常量」= 只被定义过一次，而且那一次就是整数/布尔字面量。
     * 少一个条件都不行：同一个值号可能既被常量定义、又被 存 改写。 */
    for (int v = 0; v < 值数; v++) {
        指令 *d = 汇_态.定义[v];
        if (定数[v] == 1 && d && d->码 == 指_常量 && d->文 && d->型 &&
            (d->型->种 == 型_整数 || d->型->种 == 型_布尔))
            汇_态.常值[v] = 1;
    }
    free(定数);

    /* 参数：值表最前面那些变量槽，按顺序对应形参 */
    int 参编[16];
    int 参数 = 参数个数(f, 参编);
    if (参数 > 6) { 汇_坏(f, NULL, "参数超过 6 个"); return; }
    for (int i = 0; i < 参数; i++) {
        类型 *t = 值型(f, 参编[i]);
        if (t && (t->种 == 型_字符串 || t->种 == 型_浮点 || t->种 == 型_组 ||
                  t->种 == 型_映射 || t->种 == 型_命名 || t->种 == 型_声音)) {
            汇_坏(f, NULL, "这类类型的形参");
            return;
        }
    }

    汇_算活性();
    if (汇_失败) return;
    汇_算区间();
    汇_算跨调();
    汇_分配();

    if (getenv("JY_DEBUG")) {
        fprintf(stderr, "== %.*s 值数 %d 块数 %d 稳数 %d\n", f->名长, f->名, 值数, 块数, 汇_态.稳数);
        for (int v = 0; v < 值数; v++)
            fprintf(stderr, "  v%-3d [%4d,%4d] %s %s\n", v, 汇_态.区起[v], 汇_态.区止[v],
                    汇_态.寄[v] >= 0 ? 汇_寄名[汇_态.寄[v]] : "栈",
                    f->值[v].是变量 ? "变量" : "");
    }

    /* callee-saved 顺序 */
    汇_稳总 = 0;
    for (int r = 0; r < 汇_寄数; r++)
        if (汇_态.稳用[r]) 汇_稳序[汇_稳总++] = r;

    汇_槽基 = 8 * 汇_稳总;
    for (int v = 0; v < 值数; v++) 汇_态.槽[v] = 汇_槽(v);

    int 帧 = 汇_槽基 + 16 * 值数;
    if (帧 % 16) 帧 += 8;
    帧 = (帧 + 15) & ~15;

    fprintf(出, "\n/* ---- 简言函数：%.*s（%d 块 / %d 条指令 / 稳存 %d）---- */\n",
            f->名长, f->名, 块数, 总, 汇_稳总);
    fprintf(出, "汇F%d:\n", 序);
    fputs("    pushq %rbp\n    movq %rsp, %rbp\n", 出);
    for (int i = 0; i < 汇_稳总; i++)
        fprintf(出, "    pushq %s\n", 汇_寄名[汇_稳序[i]]);
    if (帧) fprintf(出, "    subq $%d, %%rsp\n", 帧);

    /* 形参：先全部落槽，再抬进分配到的寄存器。
     * 必须先落完再抬——否则抬第一个就可能踩掉第二个的入参寄存器。 */
    static const char *参寄[6] = { "%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9" };
    for (int i = 0; i < 参数; i++)
        fprintf(出, "    movq %s, %d(%%rbp)\n", 参寄[i], 汇_槽(参编[i]));
    for (int i = 0; i < 参数; i++)
        if (汇_态.寄[参编[i]] >= 0)
            fprintf(出, "    movq %d(%%rbp), %s\n", 汇_槽(参编[i]), 汇_寄名[汇_态.寄[参编[i]]]);

    汇_态.号块 = (基本块 **)calloc((size_t)(f->下个块号 + 2), sizeof(基本块 *));
    for (int b = 0; b < 块数; b++)
        if (汇_态.块[b]->号 >= 0 && 汇_态.块[b]->号 <= f->下个块号)
            汇_态.号块[汇_态.块[b]->号] = 汇_态.块[b];

    int idx = 0;
    for (int b = 0; b < 块数; b++) {
        fprintf(出, ".L%d_%d:\n", 序, 汇_态.块[b]->号);
        汇_下块 = (b + 1 < 块数) ? 汇_态.块[b + 1] : NULL;
        for (指令 *q = 汇_态.块[b]->头; q; q = q->下) {
            汇_单条(f, 序, q, idx);
            idx++;
        }
    }
    汇_下块 = NULL;

    /* 兜底：块掉到头也没返回，就当返回 0，别让 CPU 跑飞去 */
    fputs("    xorl %eax, %eax\n", 出);
    汇_复位尾声();
    fputs("    leave\n    ret\n", 出);

    free(汇_态.槽); free(汇_态.寄); free(汇_态.用次); free(汇_态.定义); free(汇_态.常值);
    free(汇_态.块); free(汇_态.块起); free(汇_态.块条); free(汇_态.号块);
    free(汇_态.活入); free(汇_态.活出);
    free(汇_态.区起); free(汇_态.区止); free(汇_态.跨调);
}

static int 汇_序号(中间函数 *f)
{
    for (int i = 0; i < 汇_函数; i++) if (汇_函[i] == f) return i;
    return -1;
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
    if (汇_浮数) fputs("    .align 8\n", 出);
    for (int i = 0; i < 汇_浮数; i++) {
        unsigned long long u;
        memcpy(&u, &汇_浮值[i], sizeof(double));
        fprintf(出, ".LCF%d: .quad 0x%llx\n", i, u);
    }
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
            for (指令 *q = b->头; q; q = q->下)
                if (q->码 == 指_常量 && q->型 && q->文) {
                    if (q->型->种 == 型_字符串) 汇_串号(q->文, q->文长);
                    else if (q->型->种 == 型_浮点) 汇_浮号(汇_浮文(q->文, q->文长));
                }
    }

    fputs("/* ============================================================\n"
          " *  这份汇编由 简言 原生后端直接生成，不经过 C，也不经过 gcc。\n"
          " *  流程：.jy -> 词法 -> 语法 -> 语义 -> IR -> 寄存器分配 -> 这里 -> as -> ld\n"
          " * ============================================================ */\n\n", 出);
    fputs("    .text\n\n", 出);

    写运行时汇编();
    for (int i = 0; i < 汇_函数; i++) { 汇_简化(汇_函[i]); 写函数汇编(汇_函[i], i); }
    写入口汇编();
    写串池汇编();

    return !汇_失败;
}
