/* kernel/idt/isr.S - Nova OS 阶段三：中断 stub 与公共入口
 *
 * 每个向量一个 stub：
 *   - 无错误码向量：pushq $0（占位错误码），再 push 向量号
 *   - 有错误码向量（8,10,11,12,13,14,17,21,29,30）：CPU 已压错误码，只 push 向量号
 * 然后跳转 common_isr_entry：
 *   pushaq 保存全部 GPR -> call interrupt_dispatch(frame) -> popaq
 *   -> add $16（弹掉向量号+错误码）-> iretq
 *
 * isr_stub_table[256] 供 C 代码填写 IDT 表项。
 */

.section .text
.code64

/* ------------------------------------------------------------------ */
/* pushaq / popaq：保存/恢复全部通用寄存器                           */
/* ------------------------------------------------------------------ */
.macro pushaq
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rbx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
.endm

.macro popaq
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    popq %rdx
    popq %rcx
    popq %rax
.endm

/* ------------------------------------------------------------------ */
/* 向量 stub 定义宏                                                  */
/* ------------------------------------------------------------------ */
.macro ISR_NOERR n
.p2align 4
.global isr\n
.type isr\n, @function
isr\n:
    pushq $0              /* 占位错误码 */
    pushq $\n             /* 向量号 */
    jmp common_isr_entry
.size isr\n, . - isr\n
.endm

.macro ISR_ERR n
.p2align 4
.global isr\n
.type isr\n, @function
isr\n:
    pushq $\n             /* 向量号（错误码已由 CPU 压栈） */
    jmp common_isr_entry
.size isr\n, . - isr\n
.endm

/* ------------------------------------------------------------------ */
/* 二分递归生成 [from,to] 区间向量（避免 256 层递归超限）           */
/* 带错误码：8,10,11,12,13,14,17,21,29,30                            */
/* ------------------------------------------------------------------ */
.altmacro
.macro GEN_ISRS from, to
.if \from <= \to
    .if \from == \to
        .if ((\from == 8) || (\from == 10) || (\from == 11) || (\from == 12) || (\from == 13) || (\from == 14) || (\from == 17) || (\from == 21) || (\from == 29) || (\from == 30))
            ISR_ERR \from
        .else
            ISR_NOERR \from
        .endif
    .else
        GEN_ISRS \from, %((\from + \to) / 2)
        GEN_ISRS %((\from + \to) / 2 + 1), \to
    .endif
.endif
.endm

GEN_ISRS 0, 255

/* ------------------------------------------------------------------ */
/* 公共入口                                                          */
/* ------------------------------------------------------------------ */
.extern interrupt_dispatch

.p2align 4
.global common_isr_entry
.type common_isr_entry, @function
common_isr_entry:
    pushaq
    movq %rsp, %rdi       /* 第一个参数：struct isr_frame * */
    call interrupt_dispatch
    popaq
    addq $16, %rsp        /* 弹掉向量号 + 错误码 */
    iretq
.size common_isr_entry, . - common_isr_entry

/* ------------------------------------------------------------------ */
/* stub 地址表：isr_stub_table[vec] = isrN（二分递归）              */
/* ------------------------------------------------------------------ */
.section .rodata
.p2align 3
.global isr_stub_table
isr_stub_table:
.macro GEN_STUBS from, to
.if \from <= \to
    .if \from == \to
        .quad isr\from
    .else
        GEN_STUBS \from, %((\from + \to) / 2)
        GEN_STUBS %((\from + \to) / 2 + 1), \to
    .endif
.endif
.endm
GEN_STUBS 0, 255
