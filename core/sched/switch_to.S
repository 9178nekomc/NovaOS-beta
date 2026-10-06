/* kernel/sched/switch_to.S - Nova OS 阶段九：上下文切换汇编
 *
 * switch_to(prev_regs, next_regs)：
 *   1. pushfq + pushaq 将 RFLAGS 与全部 GPR 压入当前（prev）任务栈
 *   2. prev->regs.rsp = %rsp（含返回地址）
 *   3. %rsp = next->regs.rsp（切到目标任务栈）
 *   4. popaq + popfq 恢复 next 的 GPR 与 RFLAGS
 *   5. ret —— 目标任务从它上次让出 CPU 的位置继续
 *
 * 关键：RFLAGS 必须随任务保存/恢复，否则新任务首次运行会继承
 * 切换者（可能是中断上下文）的 IF=0，导致其永远收不到时钟中断
 * （时间片/睡眠唤醒失效）。
 *
 * 栈布局（低->高）：[r15..rax 15×GPR][rflags][retaddr]
 *   初始任务帧：[15×0][rflags=0x202][entry][task_trampoline]
 *     popaq 弹出 15 个零，popfq 恢复 IF=1，ret 进入 entry；
 *     entry 返回后 ret 进入蹦床（退出处理）。
 *
 * struct regs.rsp 偏移 = 128（15×8 GPR + rip 8），由 sched.c 静态断言保证。
 * 注意：rflags 由汇编栈帧传递，不进 struct regs（其 rflags 字段仅占位）。
 */

.section .text
.code64

.global switch_to
.type switch_to, @function
switch_to:
    /* pushaq：保存全部通用寄存器到 prev 栈（RFLAGS 先压，恢复时后弹） */
    pushfq
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

    /* prev->regs.rsp = %rsp（offsetof(struct regs, rsp) == 128） */
    movq %rsp, 128(%rdi)
    /* %rsp = next->regs.rsp */
    movq 128(%rsi), %rsp

    /* popaq：恢复 next 的通用寄存器 */
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
    popfq

    ret
.size switch_to, . - switch_to

/* 任务首次运行蹦床：entry 返回后进入，以 exit_code=0 退出任务 */
.global task_trampoline
.type task_trampoline, @function
task_trampoline:
    xorl %edi, %edi
    call task_exit
1:  hlt
    jmp 1b
.size task_trampoline, . - task_trampoline

/* 阶段十二：进入用户态（ring3）。
 * user_enter_asm(uint64_t user_rip, uint64_t user_rsp)
 * 在栈上构造 iretq 帧（低->高）：
 *   [user_rip][cs=0x1b][rflags=0x202][user_rsp][ss=0x23]
 * iretq 弹出后切换 CS/RPL=3、加载用户 RSP。 */
.global user_enter_asm
.type user_enter_asm, @function
user_enter_asm:
    pushq $0x23            /* SS = GDT_USER_DATA | 3 */
    pushq %rsi             /* 用户 RSP */
    pushq $0x202           /* RFLAGS：IF=1 */
    pushq $0x1b            /* CS = GDT_USER_CODE | 3 */
    pushq %rdi             /* 用户 RIP */
    iretq
.size user_enter_asm, . - user_enter_asm
