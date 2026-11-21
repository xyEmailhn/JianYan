/* ============================================================
 *  这份汇编由 简言 原生后端直接生成，不经过 C，也不经过 gcc。
 *  流程：.jy -> 词法 -> 语法 -> 语义 -> IR -> 这里 -> as -> ld
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

简_换行:
    leaq .Lnl(%rip), %rdi
    movq $1, %rsi
    jmp 简_写

简_幂:
    movq $1, %rax
    testq %rsi, %rsi
    jle .Lpow_done
    testq %rax, %rax
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

    .section .rodata
.Lnl: .byte 10

/* 堆的两个游标。放 .bss：不占文件大小，加载时是 0 */
    .bss
    .align 8
简_堆顶: .skip 8
简_堆尾: .skip 8
    .text


/* ---- 简言函数：斐波那契（4 块 / 17 条指令）---- */
汇F0:
    pushq %rbp
    movq %rsp, %rbp
    subq $288, %rsp
    movq %rdi, -16(%rbp)
.L0_0:
    movq -16(%rbp), %rax
    movq %rax, -32(%rbp)
    movq -8(%rbp), %rax
    movq %rax, -24(%rbp)
    movq $2, -48(%rbp)
    movq $0, -40(%rbp)
    movq -32(%rbp), %rax
    movq -48(%rbp), %rcx
    cmpq %rcx, %rax
    setl %al
    movzbq %al, %rax
    movq %rax, -64(%rbp)
    cmpq $0, -64(%rbp)
    jne .L0_1
    jmp .L0_2
.L0_1:
    movq -16(%rbp), %rax
    movq %rax, -80(%rbp)
    movq -8(%rbp), %rax
    movq %rax, -72(%rbp)
    movq -80(%rbp), %rax
    movq -72(%rbp), %rdx
    leave
    ret
.L0_2:
    jmp .L0_3
.L0_3:
    movq -16(%rbp), %rax
    movq %rax, -96(%rbp)
    movq -8(%rbp), %rax
    movq %rax, -88(%rbp)
    movq $1, -112(%rbp)
    movq $0, -104(%rbp)
    movq -96(%rbp), %rax
    subq -112(%rbp), %rax
    movq %rax, -128(%rbp)
    movq -128(%rbp), %rdi
    call 汇F0
    movq %rax, -144(%rbp)
    movq -16(%rbp), %rax
    movq %rax, -160(%rbp)
    movq -8(%rbp), %rax
    movq %rax, -152(%rbp)
    movq $2, -176(%rbp)
    movq $0, -168(%rbp)
    movq -160(%rbp), %rax
    subq -176(%rbp), %rax
    movq %rax, -192(%rbp)
    movq -192(%rbp), %rdi
    call 汇F0
    movq %rax, -208(%rbp)
    movq -144(%rbp), %rax
    addq -208(%rbp), %rax
    movq %rax, -224(%rbp)
    movq -224(%rbp), %rax
    movq -216(%rbp), %rdx
    leave
    ret
    xorl %eax, %eax
    leave
    ret

/* ---- 简言函数：求和（4 块 / 20 条指令）---- */
汇F1:
    pushq %rbp
    movq %rsp, %rbp
    subq $304, %rsp
    movq %rdi, -16(%rbp)
.L1_0:
    movq $0, -32(%rbp)
    movq $0, -24(%rbp)
    movq -32(%rbp), %rax
    movq %rax, -48(%rbp)
    movq -24(%rbp), %rax
    movq %rax, -40(%rbp)
    movq $1, -64(%rbp)
    movq $0, -56(%rbp)
    movq -64(%rbp), %rax
    movq %rax, -80(%rbp)
    movq -56(%rbp), %rax
    movq %rax, -72(%rbp)
    jmp .L1_1
.L1_1:
    movq -80(%rbp), %rax
    movq %rax, -96(%rbp)
    movq -72(%rbp), %rax
    movq %rax, -88(%rbp)
    movq -16(%rbp), %rax
    movq %rax, -112(%rbp)
    movq -8(%rbp), %rax
    movq %rax, -104(%rbp)
    movq -96(%rbp), %rax
    movq -112(%rbp), %rcx
    cmpq %rcx, %rax
    setle %al
    movzbq %al, %rax
    movq %rax, -128(%rbp)
    cmpq $0, -128(%rbp)
    jne .L1_2
    jmp .L1_3
.L1_2:
    movq -48(%rbp), %rax
    movq %rax, -144(%rbp)
    movq -40(%rbp), %rax
    movq %rax, -136(%rbp)
    movq -80(%rbp), %rax
    movq %rax, -160(%rbp)
    movq -72(%rbp), %rax
    movq %rax, -152(%rbp)
    movq -144(%rbp), %rax
    addq -160(%rbp), %rax
    movq %rax, -176(%rbp)
    movq -176(%rbp), %rax
    movq %rax, -48(%rbp)
    movq -168(%rbp), %rax
    movq %rax, -40(%rbp)
    movq -80(%rbp), %rax
    movq %rax, -192(%rbp)
    movq -72(%rbp), %rax
    movq %rax, -184(%rbp)
    movq $1, -208(%rbp)
    movq $0, -200(%rbp)
    movq -192(%rbp), %rax
    addq -208(%rbp), %rax
    movq %rax, -224(%rbp)
    movq -224(%rbp), %rax
    movq %rax, -80(%rbp)
    movq -216(%rbp), %rax
    movq %rax, -72(%rbp)
    jmp .L1_1
.L1_3:
    movq -48(%rbp), %rax
    movq %rax, -240(%rbp)
    movq -40(%rbp), %rax
    movq %rax, -232(%rbp)
    movq -240(%rbp), %rax
    movq -232(%rbp), %rdx
    leave
    ret
    xorl %eax, %eax
    leave
    ret

/* ---- 简言函数：主（4 块 / 52 条指令）---- */
汇F2:
    pushq %rbp
    movq %rsp, %rbp
    subq $816, %rsp
.L2_0:
    leaq .LSTR0(%rip), %rax
    movq %rax, -16(%rbp)
    movq $19, -8(%rbp)
    movq -16(%rbp), %rdi
    movq -8(%rbp), %rsi
    call 简_写
    movq $0, -32(%rbp)
    movq $20, -48(%rbp)
    movq $0, -40(%rbp)
    movq -48(%rbp), %rdi
    call 汇F0
    movq %rax, -64(%rbp)
    movq -64(%rbp), %rdi
    call 简_打整
    call 简_换行
    movq $0, -80(%rbp)
    leaq .LSTR1(%rip), %rax
    movq %rax, -96(%rbp)
    movq $16, -88(%rbp)
    movq -96(%rbp), %rdi
    movq -88(%rbp), %rsi
    call 简_写
    movq $0, -112(%rbp)
    movq $100, -128(%rbp)
    movq $0, -120(%rbp)
    movq -128(%rbp), %rdi
    call 汇F1
    movq %rax, -144(%rbp)
    movq -144(%rbp), %rdi
    call 简_打整
    call 简_换行
    movq $0, -160(%rbp)
    leaq .LSTR2(%rip), %rax
    movq %rax, -176(%rbp)
    movq $7, -168(%rbp)
    movq -176(%rbp), %rdi
    movq -168(%rbp), %rsi
    call 简_写
    movq $0, -192(%rbp)
    movq $1024, -208(%rbp)
    movq $0, -200(%rbp)
    movq -208(%rbp), %rdi
    call 简_打整
    call 简_换行
    movq $0, -224(%rbp)
    leaq .LSTR3(%rip), %rax
    movq %rax, -240(%rbp)
    movq $10, -232(%rbp)
    movq -240(%rbp), %rdi
    movq -232(%rbp), %rsi
    call 简_写
    movq $0, -256(%rbp)
    movq $3, -272(%rbp)
    movq $0, -264(%rbp)
    movq -272(%rbp), %rdi
    call 简_打整
    call 简_换行
    movq $0, -288(%rbp)
    leaq .LSTR4(%rip), %rax
    movq %rax, -304(%rbp)
    movq $9, -296(%rbp)
    movq -304(%rbp), %rdi
    movq -296(%rbp), %rsi
    call 简_写
    movq $0, -320(%rbp)
    movq $2, -336(%rbp)
    movq $0, -328(%rbp)
    movq -336(%rbp), %rdi
    call 简_打整
    call 简_换行
    movq $0, -352(%rbp)
    leaq .LSTR5(%rip), %rax
    movq %rax, -368(%rbp)
    movq $24, -360(%rbp)
    movq -368(%rbp), %rdi
    movq -360(%rbp), %rsi
    call 简_写
    movq $0, -384(%rbp)
    movq $-3, -400(%rbp)
    movq $0, -392(%rbp)
    movq -400(%rbp), %rdi
    call 简_打整
    call 简_换行
    movq $0, -416(%rbp)
    leaq .LSTR6(%rip), %rax
    movq %rax, -432(%rbp)
    movq $23, -424(%rbp)
    movq -432(%rbp), %rdi
    movq -424(%rbp), %rsi
    call 简_写
    movq $0, -448(%rbp)
    movq $-2, -464(%rbp)
    movq $0, -456(%rbp)
    movq -464(%rbp), %rdi
    call 简_打整
    call 简_换行
    movq $0, -480(%rbp)
    leaq .LSTR7(%rip), %rax
    movq %rax, -496(%rbp)
    movq $9, -488(%rbp)
    movq -496(%rbp), %rdi
    movq -488(%rbp), %rsi
    call 简_写
    movq $0, -512(%rbp)
    leaq .LSTR8(%rip), %rax
    movq %rax, -528(%rbp)
    movq $7, -520(%rbp)
    movq -528(%rbp), %rdi
    movq -520(%rbp), %rsi
    call 简_写
    call 简_换行
    movq $0, -544(%rbp)
    movq $0, -560(%rbp)
    movq $0, -552(%rbp)
    movq -560(%rbp), %rax
    movq %rax, -576(%rbp)
    movq -552(%rbp), %rax
    movq %rax, -568(%rbp)
    jmp .L2_1
.L2_1:
    movq -576(%rbp), %rax
    movq %rax, -592(%rbp)
    movq -568(%rbp), %rax
    movq %rax, -584(%rbp)
    movq $10, -608(%rbp)
    movq $0, -600(%rbp)
    movq -592(%rbp), %rax
    movq -608(%rbp), %rcx
    cmpq %rcx, %rax
    setl %al
    movzbq %al, %rax
    movq %rax, -624(%rbp)
    cmpq $0, -624(%rbp)
    jne .L2_2
    jmp .L2_3
.L2_2:
    movq -576(%rbp), %rax
    movq %rax, -640(%rbp)
    movq -568(%rbp), %rax
    movq %rax, -632(%rbp)
    movq $3, -656(%rbp)
    movq $0, -648(%rbp)
    movq -640(%rbp), %rax
    addq -656(%rbp), %rax
    movq %rax, -672(%rbp)
    movq -672(%rbp), %rax
    movq %rax, -576(%rbp)
    movq -664(%rbp), %rax
    movq %rax, -568(%rbp)
    jmp .L2_1
.L2_3:
    leaq .LSTR9(%rip), %rax
    movq %rax, -688(%rbp)
    movq $6, -680(%rbp)
    movq -688(%rbp), %rdi
    movq -680(%rbp), %rsi
    call 简_写
    movq $0, -704(%rbp)
    movq -576(%rbp), %rax
    movq %rax, -720(%rbp)
    movq -568(%rbp), %rax
    movq %rax, -712(%rbp)
    movq -720(%rbp), %rdi
    call 简_打整
    call 简_换行
    movq $0, -736(%rbp)
    movq $0, -752(%rbp)
    movq $0, -744(%rbp)
    movq -752(%rbp), %rax
    movq -744(%rbp), %rdx
    leave
    ret
    xorl %eax, %eax
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
