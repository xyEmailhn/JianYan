/* ============================================================
 *  这份汇编由 简言 原生后端直接生成，不经过 C，也不经过 gcc。
 *  流程：.jy -> 词法 -> 语法 -> 语义 -> IR -> 寄存器分配 -> 这里 -> as -> ld
 * ============================================================ */

    .text

/* ---- 运行时 ---- */
简_写:
    movq %rsi, %rdx
    movq %rdi, %rsi
    movl $1, %edi
    movl $1, %eax
    syscall
    ret

简_打整:
    pushq %rbp
    movq %rsp, %rbp
    subq $48, %rsp
    movq %rdi, %rax
    leaq -1(%rbp), %rsi
    xorq %rcx, %rcx
    movq $10, %r8
    xorq %r9, %r9
    testq %rax, %rax
    jns .Lnum_start
    negq %rax
    movq $1, %r9
.Lnum_start:
    testq %rax, %rax
    jnz .Lnum_loop
    decq %rsi
    movb $48, (%rsi)
    incq %rcx
    jmp .Lnum_sign
.Lnum_loop:
    xorq %rdx, %rdx
    divq %r8
    addb $48, %dl
    decq %rsi
    movb %dl, (%rsi)
    incq %rcx
    testq %rax, %rax
    jnz .Lnum_loop
.Lnum_sign:
    testq %r9, %r9
    jz .Lnum_out
    decq %rsi
    movb $45, (%rsi)
    incq %rcx
.Lnum_out:
    movq %rsi, %rdi
    movq %rcx, %rsi
    call 简_写
    leave
    ret

/* 简_字数(rdi=字节, rsi=长) → rax = 字符数：数非续字节 */
简_字数:
    xorq %rax, %rax
    xorq %rcx, %rcx
.Lcnt_loop:
    cmpq %rsi, %rcx
    jge .Lcnt_done
    movb (%rdi,%rcx), %dl
    andb $0xc0, %dl
    cmpb $0x80, %dl
    je .Lcnt_next
    incq %rax
.Lcnt_next:
    incq %rcx
    jmp .Lcnt_loop
.Lcnt_done:
    ret

简_换行:
    leaq .Lnl(%rip), %rdi
    movq $1, %rsi
    jmp 简_写

简_幂:
    movq $1, %rax
    testq %rsi, %rsi
    jle .Lpow_done
.Lpow_loop:
    testq $1, %rsi
    jz .Lpow_skip
    imulq %rdi, %rax
.Lpow_skip:
    imulq %rdi, %rdi
    shrq $1, %rsi
    jnz .Lpow_loop
.Lpow_done:
    ret

/* 堆：顶/尾两个全局。第一次用时 mmap 1MB，之后只挪指针。 */
简_分配:
    movq %rdi, %rcx
    movq 简_堆顶(%rip), %rax
    leaq (%rax,%rcx), %rdx
    cmpq 简_堆尾(%rip), %rdx
    jbe .Lalloc_ok
    pushq %rdi
    xorq %rdi, %rdi
    movq $1048576, %rsi
    movq $3, %rdx
    movq $34, %r10
    movq $-1, %r8
    xorq %r9, %r9
    movl $9, %eax
    syscall
    popq %rdi
    movq %rax, 简_堆顶(%rip)
    movq %rax, %rdx
    addq $1048576, %rdx
    movq %rdx, 简_堆尾(%rip)
    leaq (%rax,%rdi), %rdx
    movq %rdi, %rcx
.Lalloc_ok:
    movq %rdx, 简_堆顶(%rip)
    ret

/* 简_拼(rdi=串1, rsi=长1, rdx=串2, rcx=长2) */
简_拼:
    pushq %rbp
    movq %rsp, %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    movq %rdi, %rbx
    movq %rsi, %r12
    movq %rdx, %r13
    movq %rcx, %r14
    leaq 1(%rsi,%rcx), %rdi
    call 简_分配
    movq %rax, %rdi
    movq %rax, %r8
    movq %rbx, %rsi
    movq %r12, %rcx
    rep movsb
    movq %r13, %rsi
    movq %r14, %rcx
    rep movsb
    movb $0, (%rdi)
    movq %r8, %rax
    movq %r12, %rdx
    addq %r14, %rdx
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    leave
    ret

/* ---- 简_打浮：xmm0 里的 double，按 %g 的样子写出来 ----
 * 6 位有效数字、去尾零；指数 < -4 或 >= 6 走科学计数法。
 * 只用 SSE 和整数除法，不碰 libc。 */
简_打浮:
    pushq %rbp
    movq %rsp, %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    subq $128, %rsp
    leaq -33(%rbp), %rbx
    leaq -160(%rbp), %r12
    movq %xmm0, %rax
    shrq $63, %rax
    movq %rax, %r13
    testq %r13, %r13
    jz .Lpf_pos
    movq %xmm0, %rax
    btrq $63, %rax
    movq %rax, %xmm0
.Lpf_pos:
    xorpd %xmm1, %xmm1
    ucomisd %xmm1, %xmm0
    jne .Lpf_nz
    movb $48, -34(%rbp)
    leaq -34(%rbp), %rbx
    jmp .Lpf_emit
.Lpf_nz:
    xorl %r9d, %r9d
    movsd .LCF十(%rip), %xmm2
.Lpf_big:
    ucomisd %xmm2, %xmm0
    jb .Lpf_small
    divsd %xmm2, %xmm0
    incq %r9
    jmp .Lpf_big
.Lpf_small:
    movsd .LCF一(%rip), %xmm2
.Lpf_sm2:
    ucomisd %xmm2, %xmm0
    jae .Lpf_norm
    movsd .LCF十(%rip), %xmm3
    mulsd %xmm3, %xmm0
    decq %r9
    jmp .Lpf_sm2
.Lpf_norm:
    movsd .LCF十万(%rip), %xmm2
    mulsd %xmm2, %xmm0
    cvtsd2si %xmm0, %rax
    cmpq $1000000, %rax
    jl .Lpf_dok
    movq $100000, %rax
    incq %r9
.Lpf_dok:
    movl $5, %ecx
    movq $10, %r8
.Lpf_dig:
    xorq %rdx, %rdx
    divq %r8
    addb $48, %dl
    movb %dl, (%r12,%rcx)
    decq %rcx
    jns .Lpf_dig
    cmpq $-4, %r9
    jl .Lpf_exp
    cmpq $6, %r9
    jge .Lpf_exp
    cmpq $0, %r9
    jl .Lpf_nege
    movq $5, %r10
.Lpf_a1:
    cmpq %r9, %r10
    jle .Lpf_a2
    cmpb $48, (%r12,%r10)
    jne .Lpf_a2
    decq %r10
    jmp .Lpf_a1
.Lpf_a2:
    cmpq %r9, %r10
    jle .Lpf_a3
    movq %r10, %rcx
.Lpf_a4:
    movb (%r12,%rcx), %al
    decq %rbx
    movb %al, (%rbx)
    decq %rcx
    cmpq %r9, %rcx
    jg .Lpf_a4
    movb $46, %al
    decq %rbx
    movb %al, (%rbx)
.Lpf_a3:
    movq %r9, %rcx
.Lpf_a5:
    movb (%r12,%rcx), %al
    decq %rbx
    movb %al, (%rbx)
    decq %rcx
    jns .Lpf_a5
    jmp .Lpf_emit
.Lpf_nege:
    movq $5, %r10
.Lpf_b1:
    cmpb $48, (%r12,%r10)
    jne .Lpf_b2
    decq %r10
    jns .Lpf_b1
.Lpf_b2:
    movq %r10, %rcx
.Lpf_b3:
    movb (%r12,%rcx), %al
    decq %rbx
    movb %al, (%rbx)
    decq %rcx
    jns .Lpf_b3
    movq %r9, %rcx
    negq %rcx
    decq %rcx
.Lpf_b4:
    testq %rcx, %rcx
    jle .Lpf_b5
    movb $48, %al
    decq %rbx
    movb %al, (%rbx)
    decq %rcx
    jmp .Lpf_b4
.Lpf_b5:
    movb $46, %al
    decq %rbx
    movb %al, (%rbx)
    movb $48, %al
    decq %rbx
    movb %al, (%rbx)
    jmp .Lpf_emit
.Lpf_exp:
    movq $5, %r10
.Lpf_c1:
    cmpb $48, (%r12,%r10)
    jne .Lpf_c2
    decq %r10
    jns .Lpf_c1
.Lpf_c2:
    movq %r9, %r11
    testq %r11, %r11
    jns .Lpf_c6
    negq %r11
.Lpf_c6:
    movq %r11, %rax
    movq $10, %r8
    xorq %rdx, %rdx
    divq %r8
    addb $48, %dl
    decq %rbx
    movb %dl, (%rbx)
    movq %rax, %rcx
    testq %rcx, %rcx
    jnz .Lpf_c7
    movb $48, %al
    decq %rbx
    movb %al, (%rbx)
    jmp .Lpf_c8
.Lpf_c7:
    xorq %rdx, %rdx
    movq %rcx, %rax
    divq %r8
    addb $48, %dl
    decq %rbx
    movb %dl, (%rbx)
    movq %rax, %rcx
    testq %rcx, %rcx
    jnz .Lpf_c7
.Lpf_c8:
    testq %r9, %r9
    jns .Lpf_c9
    movb $45, %al
    jmp .Lpf_c10
.Lpf_c9:
    movb $43, %al
.Lpf_c10:
    decq %rbx
    movb %al, (%rbx)
    movb $101, %al
    decq %rbx
    movb %al, (%rbx)
    cmpq $0, %r10
    jle .Lpf_c13
    movq %r10, %rcx
.Lpf_c12:
    movb (%r12,%rcx), %al
    decq %rbx
    movb %al, (%rbx)
    decq %rcx
    cmpq $0, %rcx
    jg .Lpf_c12
    movb $46, %al
    decq %rbx
    movb %al, (%rbx)
.Lpf_c13:
    movb (%r12), %al
    decq %rbx
    movb %al, (%rbx)
    jmp .Lpf_emit
.Lpf_emit:
    testq %r13, %r13
    jz .Lpf_go
    movb $45, %al
    decq %rbx
    movb %al, (%rbx)
.Lpf_go:
    movq %rbx, %rdi
    leaq -33(%rbp), %rsi
    subq %rbx, %rsi
    call 简_写
    addq $128, %rsp
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    leave
    ret

    .section .rodata
.Lnl: .byte 10
    .align 8
.LCF一: .quad 0x3ff0000000000000
.LCF十: .quad 0x4024000000000000
.LCF十万: .quad 0x40f86a0000000000

/* 堆的两个游标。放 .bss：不占文件大小，加载时是 0 */
    .bss
    .align 8
简_堆顶: .skip 8
简_堆尾: .skip 8
    .text


/* ---- 简言函数：斐波那契（4 块 / 17 条指令 / 稳存 2）---- */
汇F0:
    pushq %rbp
    movq %rsp, %rbp
    pushq %rbx
    pushq %r12
    subq $240, %rsp
    movq %rdi, -32(%rbp)
    movq -32(%rbp), %rbx
.L0_0:
    cmpq $2, %rbx
    jge .L0_2
.L0_1:
    movq %rbx, %rax
    movq -8(%rbp), %rbx
    movq -16(%rbp), %r12
    leave
    ret
.L0_2:
.L0_3:
    leaq -1(%rbx), %rsi
    movq %rsi, %rdi
    call 汇F0
    movq %rax, %r12
    subq $2, %rbx
    movq %rbx, %rdi
    call 汇F0
    movq %rax, %rbx
    addq %rbx, %r12
    movq %r12, %rax
    movq -8(%rbp), %rbx
    movq -16(%rbp), %r12
    leave
    ret
    xorl %eax, %eax
    movq -8(%rbp), %rbx
    movq -16(%rbp), %r12
    leave
    ret

/* ---- 简言函数：求和（4 块 / 20 条指令 / 稳存 1）---- */
汇F1:
    pushq %rbp
    movq %rsp, %rbp
    pushq %rbx
    subq $256, %rsp
    movq %rdi, -24(%rbp)
    movq -24(%rbp), %rbx
.L1_0:
    movq $0, %r10
    movq $1, %rsi
.L1_1:
    cmpq %rbx, %rsi
    jg .L1_3
.L1_2:
    addq %rsi, %r10
    addq $1, %rsi
    jmp .L1_1
.L1_3:
    movq %r10, %rax
    movq -8(%rbp), %rbx
    leave
    ret
    xorl %eax, %eax
    movq -8(%rbp), %rbx
    leave
    ret

/* ---- 简言函数：主（4 块 / 52 条指令 / 稳存 3）---- */
汇F2:
    pushq %rbp
    movq %rsp, %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    subq $784, %rsp
.L2_0:
    leaq .LSTR0(%rip), %rsi
    movq $19, -32(%rbp)
    movq %rsi, %rdi
    movq -32(%rbp), %rsi
    call 简_写
    movq $20, %rdi
    call 汇F0
    movq %rax, %rsi
    movq %rsi, %rdi
    call 简_打整
    call 简_换行
    leaq .LSTR1(%rip), %rsi
    movq $16, -112(%rbp)
    movq %rsi, %rdi
    movq -112(%rbp), %rsi
    call 简_写
    movq $100, %rdi
    call 汇F1
    movq %rax, %rsi
    movq %rsi, %rdi
    call 简_打整
    call 简_换行
    leaq .LSTR2(%rip), %rsi
    movq $7, -192(%rbp)
    movq %rsi, %rdi
    movq -192(%rbp), %rsi
    call 简_写
    movq $1024, %rdi
    call 简_打整
    call 简_换行
    leaq .LSTR3(%rip), %rsi
    movq $10, -256(%rbp)
    movq %rsi, %rdi
    movq -256(%rbp), %rsi
    call 简_写
    movq $3, %rdi
    call 简_打整
    call 简_换行
    leaq .LSTR4(%rip), %rsi
    movq $9, -320(%rbp)
    movq %rsi, %rdi
    movq -320(%rbp), %rsi
    call 简_写
    movq $2, %rdi
    call 简_打整
    call 简_换行
    leaq .LSTR5(%rip), %rsi
    movq $24, -384(%rbp)
    movq %rsi, %rdi
    movq -384(%rbp), %rsi
    call 简_写
    movq $-3, %rdi
    call 简_打整
    call 简_换行
    leaq .LSTR6(%rip), %rsi
    movq $23, -448(%rbp)
    movq %rsi, %rdi
    movq -448(%rbp), %rsi
    call 简_写
    movq $-2, %rdi
    call 简_打整
    call 简_换行
    leaq .LSTR7(%rip), %rsi
    movq $9, -512(%rbp)
    movq %rsi, %rdi
    movq -512(%rbp), %rsi
    call 简_写
    leaq .LSTR8(%rip), %rsi
    movq $7, -544(%rbp)
    movq %rsi, %rdi
    movq -544(%rbp), %rsi
    call 简_写
    call 简_换行
    movq $0, %r13
.L2_1:
    cmpq $10, %r13
    jge .L2_3
.L2_2:
    addq $3, %r13
    jmp .L2_1
.L2_3:
    leaq .LSTR9(%rip), %rbx
    movq $6, -704(%rbp)
    movq %rbx, %rdi
    movq -704(%rbp), %rsi
    call 简_写
    movq %r13, %rdi
    call 简_打整
    call 简_换行
    movq $0, %rax
    movq -8(%rbp), %rbx
    movq -16(%rbp), %r12
    movq -24(%rbp), %r13
    leave
    ret
    xorl %eax, %eax
    movq -8(%rbp), %rbx
    movq -16(%rbp), %r12
    movq -24(%rbp), %r13
    leave
    ret

/* ---- 入口：_start 直接 syscall exit，不经过 libc ---- */
    .globl _start
_start:
    andq $-16, %rsp
    call 汇F2
    movslq %eax, %rdi
    movq $60, %rax
    syscall

    .section .rodata
.LTRUE:  .byte 231,156,159
.LFALSE: .byte 229,129,135
.LSTR0:
    .byte 230,150,144,230,179,162,233,130,163,229,165,145,40,50,48,41,32,61,32
.LSTR1:
    .byte 49,46,46,49,48,48,32,230,177,130,229,146,140,32,61,32
.LSTR2:
    .byte 50,94,49,48,32,61,32
.LSTR3:
    .byte 49,55,32,47,47,32,53,32,61,32
.LSTR4:
    .byte 49,55,32,37,32,53,32,61,32
.LSTR5:
    .byte 232,180,159,230,149,176,230,149,180,233,153,164,32,45,49,55,32,47,47,32,53,32,61,32
.LSTR6:
    .byte 232,180,159,230,149,176,229,143,150,228,189,153,32,45,49,55,32,37,32,53,32,61,32
.LSTR7:
    .byte 230,139,188,230,142,165,239,188,154
.LSTR8:
    .byte 231,174,128,232,168,128,33
.LSTR9:
    .byte 229,164,167,32,61,32
