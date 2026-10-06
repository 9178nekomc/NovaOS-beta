/* boot/boot.S - Nova OS 阶段一：启动汇编入口
 *
 * 由 Limine v8.x 引导器直接以 64 位长模式进入本入口：
 *   - CPU 已处于长模式，分页已开启（Limine 负责）
 *   - RSP 未定义：必须由内核自行设置栈
 *   - 无需触碰 BIOS/VGA，图形由 GOP 帧缓冲提供
 *
 * 职责：
 *   1. 设置 64KB 内核栈（.bss 中的 stack_bottom .. stack_top）
 *   2. 按 SYSV ABI 清零 RBP、对齐 RSP 到 16 字节
 *   3. 调用 C 入口 kmain()
 *   4. kmain 返回后停机（正常情况下不会返回）
 */

.section .text
.code64
.global _start
.type _start, @function
_start:
    /* 设置栈顶：栈底在低地址，栈向低地址生长，RSP 指向 stack_top */
    movabs $stack_top, %rsp

    /* SYSV ABI：RBP 清零，标记栈帧末端 */
    xorq %rbp, %rbp

    /* 调用前保证 16 字节对齐 */
    andq $-16, %rsp

    /* 启用 SSE：
     * Limine 引导协议规定进入内核时 SSE 处于关闭状态
     * （CR0.EM=1, CR4.OSFXSR=0）。而 gcc -O2 会生成 SSE 指令，
     * 不启用 SSE 会在首次使用 xmm 寄存器时触发 #UD 异常。
     * 步骤：
     *   1) CR0: 清 EM(bit2)，置 MP(bit1)
     *   2) CR4: 置 OSFXSR(bit9) 与 OSXMMEXCPT(bit10)
     *   3) 初始化 MXCSR 为默认值 0x1F80（清零所有异常掩码位保持默认）
     */
    movq %cr0, %rax
    andq $~0x4, %rax            /* 清 CR0.EM */
    orq  $0x2, %rax             /* 置 CR0.MP */
    movq %rax, %cr0

    movq %cr4, %rax
    orq  $0x600, %rax           /* 置 CR4.OSFXSR | CR4.OSXMMEXCPT */
    movq %rax, %cr4

    /* 初始化 MXCSR 为复位默认值（0x1F80） */
    movl $0x1F80, %eax
    movq %rax, %rdx
    pushq %rdx
    ldmxcsr (%rsp)
    addq $8, %rsp

    /* 进入 C 内核入口 */
    call kmain

    /* kmain 返回（不应发生）：关中断后死循环 */
1:  cli
    hlt
    jmp 1b

.size _start, . - _start

/* ------------------------------------------------------------------ */
/* 内核栈：64KB，位于 .bss                                        */
/* ------------------------------------------------------------------ */
.section .bss
.align 16
.global stack_bottom
stack_bottom:
    .skip 65536
.global stack_top
stack_top:
