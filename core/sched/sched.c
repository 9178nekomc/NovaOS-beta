/* kernel/sched/sched.c - Nova OS 阶段九/十/十一：任务管理与调度实现（多核）
 *
 * 每-CPU 独立调度：
 *   - 每个 CPU 一份就绪/睡眠队列与 current/idle（见 per_cpu.h）
 *   - schedule() 把本 CPU 当前任务放回本 CPU 队列尾，取队首切换（轮转）
 *   - 抢占：本 CPU 的 LAPIC 定时器 tick（sched_tick）递减当前任务时间片，
 *     耗尽置 need_resched 并在中断上下文调用 schedule() 抢占（10ms 片）
 *   - 睡眠：sleep() 置 BLOCKED 入本 CPU 睡眠队列；到期由 tick 唤醒
 *   - 退出：entry 返回 -> 蹦床 -> task_exit -> 置 ZOMBIE 并切走（永不返回）
 *
 * 关键实现点：
 *   - switch_to 用 pushfq/pushaq 保存 RFLAGS 与全部 GPR（否则新任务
 *     首跑会继承切换者——可能是中断上下文——的 IF=0，导致永远收不到
 *     时钟中断）。
 *   - schedule() 入口 cli/出口 sti，防止本 CPU 的 tick 在队列操作/切换
 *     中途重入（否则 current 会被嵌套 enqueue 两次，就绪队列出现重复项）。
 *   - enqueue/dequeue 用每-CPU 自旋锁：BSP 可跨 CPU 向 AP 队列入队
 *     （create_task_on），与 AP 自身的调度互斥。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "sched.h"
#include "../smp/per_cpu.h"
#include "../types/assert.h"
#include "../../lib/io.h"
#include "../../lib/uart.h"
#include "../mm/kmalloc.h"
#include "../timer/timer.h"
#include "../gdt/tss.h"

/* switch_to 依赖 regs.rsp 位于偏移 128 */
_Static_assert(offsetof(struct regs, rsp) == 128,
               "regs.rsp must be at offset 128 for switch_to");

/* 启动栈顶（boot.S .bss；idle 任务借用） */
extern uint64_t stack_top[];

static uint64_t next_pid = 1;
static struct task *all_tasks;      /* 阶段二十：全局任务注册表 */
static uint32_t all_count;

static void enqueue(struct per_cpu *pc, struct task *t)
{
    spin_lock(&pc->queue_lock);
    t->next = NULL;
    if (pc->ready_tail != NULL)
        pc->ready_tail->next = t;
    else
        pc->ready_head = t;
    pc->ready_tail = t;
    spin_unlock(&pc->queue_lock);
}

static struct task *dequeue(struct per_cpu *pc)
{
    spin_lock(&pc->queue_lock);
    struct task *t = pc->ready_head;
    if (t != NULL) {
        pc->ready_head = t->next;
        if (pc->ready_head == NULL)
            pc->ready_tail = NULL;
        t->next = NULL;
    }
    spin_unlock(&pc->queue_lock);
    return t;
}

static void wake_sleepers(struct per_cpu *pc, uint64_t now)
{
    struct task **pp = &pc->sleep_head;
    while (*pp != NULL) {
        struct task *t = *pp;
        if (t->sleep_until <= now) {
            *pp = t->sleep_next;
            t->sleep_next = NULL;
            t->state = TASK_READY;
            enqueue(pc, t);
        } else {
            pp = &t->sleep_next;
        }
    }
}

/* 加入全局任务注册表（不重复） */
static void register_task(struct task *t)
{
    if (t->all_next != NULL || t == all_tasks)
        return;
    t->all_next = all_tasks;
    all_tasks = t;
    all_count++;
}

int task_init(void)
{
    /* BSP 空闲任务：PID 0，借用启动栈（kernel_stack = 0） */
    struct per_cpu *pc = &per_cpus[0];
    struct task *t = kcalloc(1, sizeof(struct task));
    assert(t != NULL);
    t->pid = 0;
    t->state = TASK_READY;
    t->kernel_stack = 0;
    t->mm = NULL;
    t->exit_code = 0;
    t->time_slice = TASK_TIME_SLICE;
    t->sleep_until = 0;
    t->cpu = 0;
    t->next = NULL;
    t->sleep_next = NULL;
    strcpy(t->name, "idle");
    register_task(t);
    pc->current = t;
    pc->idle_task = t;
    pc->ready_head = NULL;
    pc->ready_tail = NULL;
    pc->sleep_head = NULL;
    pc->need_resched = false;
    next_pid = 1;
    return 0;
}

struct task *create_task_on(uint32_t cpu, void (*entry)(void))
{
    struct task *t = kcalloc(1, sizeof(struct task));
    assert(t != NULL);
    uint8_t *stack = kmalloc(TASK_STACK_SIZE);
    assert(stack != NULL);

    t->pid = next_pid++;
    t->state = TASK_READY;
    t->kernel_stack = (uint64_t)(uintptr_t)stack;
    t->mm = NULL;
    t->exit_code = 0;
    t->time_slice = TASK_TIME_SLICE;
    t->sleep_until = 0;
    t->cpu = cpu;
    t->sleep_next = NULL;
    strcpy(t->name, "task");
    register_task(t);

    /* 初始栈帧（低->高）：[r15..rax 15×0][rflags=0x202][entry][task_trampoline]
     * switch_to popaq 弹出 15 个零，popfq 恢复 IF=1，ret 进入 entry；
     * entry 返回后 ret 进入蹦床（退出处理）。 */
    uint64_t *sp = (uint64_t *)(uintptr_t)(stack + TASK_STACK_SIZE);
    *--sp = (uint64_t)(uintptr_t)task_trampoline;   /* 最高：entry 返回后 */
    *--sp = (uint64_t)(uintptr_t)entry;             /* ret 目标 */
    *--sp = 0x202;                                  /* RFLAGS：IF=1（popfq） */
    for (int i = 0; i < 15; i++)                    /* rax..r15 槽 */
        *--sp = 0;

    t->regs.rsp = (uint64_t)sp;
    t->regs.rip = (uint64_t)(uintptr_t)entry;
    t->regs.rflags = 0x202;              /* IF=1, 保留位 */

    /* 跨 CPU 入队：cli 防止本 CPU tick 重入导致死锁（自旋锁 + 中断） */
    cli();
    enqueue(&per_cpus[cpu], t);
    sti();
    return t;
}

struct task *create_task(void (*entry)(void))
{
    return create_task_on(0, entry);
}

void task_set_name(struct task *t, const char *name)
{
    if (t == NULL || name == NULL)
        return;
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';
}

struct task *create_named_task(const char *name, void (*entry)(void))
{
    struct task *t = create_task(entry);
    if (t != NULL)
        task_set_name(t, name);
    return t;
}

uint32_t sched_count(void)
{
    return all_count;
}

struct task *sched_get(uint32_t i)
{
    struct task *t = all_tasks;
    while (t != NULL && i-- > 0)
        t = t->all_next;
    return t;
}

int sched_kill(uint64_t pid)
{
    if (pid == 0)
        return -EPERM;          /* 不允许杀 idle */
    struct task *cur = this_cpu()->current;
    if (cur != NULL && pid == cur->pid)
        return -EPERM;          /* 不允许杀 Shell 自身 */

    struct task *t = NULL;
    for (uint32_t i = 0; i < all_count; i++) {
        struct task *c = sched_get(i);
        if (c != NULL && c->pid == pid) {
            t = c;
            break;
        }
    }
    if (t == NULL)
        return -ENOENT;
    if (t->state == TASK_ZOMBIE)
        return -ENOENT;         /* 已退出 */

    /* 运行中的任务：置 kill_pending，其 sched_tick 自行退出 */
    if (t->state == TASK_RUNNING) {
        t->kill_pending = 1;
        return 0;
    }

    /* 就绪/睡眠队列中的任务：加锁摘除 */
    cli();
    for (uint32_t c = 0; c < cpu_count; c++) {
        struct per_cpu *pc = &per_cpus[c];
        spin_lock(&pc->queue_lock);

        struct task **pp = &pc->ready_head;
        while (*pp != NULL) {
            if (*pp == t) {
                *pp = t->next;
                if (pc->ready_tail == t)
                    pc->ready_tail = NULL;
                t->state = TASK_ZOMBIE;
                t->next = NULL;
                break;
            }
            pp = &(*pp)->next;
        }
        if (t->state != TASK_ZOMBIE) {
            pp = &pc->sleep_head;
            while (*pp != NULL) {
                if (*pp == t) {
                    *pp = t->sleep_next;
                    t->state = TASK_ZOMBIE;
                    t->sleep_next = NULL;
                    break;
                }
                pp = &(*pp)->sleep_next;
            }
        }
        spin_unlock(&pc->queue_lock);
        if (t->state == TASK_ZOMBIE)
            break;
    }
    sti();
    return (t->state == TASK_ZOMBIE) ? 0 : -EBUSY;
}

/* 阶段十二：用户任务首次运行的入口（内核态），iretq 进入 ring3 */
static void user_enter(void);

struct task *create_user_task(uint64_t user_entry, uint64_t user_stack_top)
{
    struct task *t = create_task_on(0, user_enter);
    t->user_entry = user_entry;
    t->user_stack_top = user_stack_top;
    return t;
}

/* 进入用户态：构造 iretq 帧 [rip][cs=0x1b][rflags=0x202][rsp][ss=0x23] */
static void user_enter(void)
{
    struct task *t = current_task();
    /* user_enter_asm 定义于 switch_to.S */
    extern void user_enter_asm(uint64_t user_rip, uint64_t user_rsp);
    user_enter_asm(t->user_entry, t->user_stack_top);
    /* 若用户程序返回（异常路径），退出任务 */
    task_exit(0);
}

void schedule(void)
{
    /* 关闭中断：防止本 CPU 的 tick 在队列操作/切换中途重入 schedule()
     * （否则 current 会被嵌套 enqueue 两次，就绪队列出现重复项）。 */
    cli();

    struct per_cpu *pc = this_cpu();
    struct task *prev = pc->current;

    /* 轮转：运行中的任务放回队尾 */
    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
        enqueue(pc, prev);
    }

    struct task *next = dequeue(pc);
    if (next == NULL)
        next = pc->idle_task;           /* 本 CPU 就绪队列空：回 idle */

    next->state = TASK_RUNNING;
    next->time_slice = TASK_TIME_SLICE; /* 重置新任务时间片 */
    pc->current = next;

    /* 阶段十二：更新 TSS.RSP0 = 新任务的内核栈顶（ring3->ring0 用）。
     * 仅 BSP 需要（用户任务只在 BSP 运行）；idle 借用启动栈。 */
    if (pc->cpu_id == 0) {
        uint64_t ktop = next->kernel_stack
                            ? next->kernel_stack + TASK_STACK_SIZE
                            : (uint64_t)(uintptr_t)stack_top;
        tss_set_rsp0(ktop);
    }

    if (prev != next)
        switch_to(&prev->regs, &next->regs);
    /* 返回：本任务再次被调度，继续执行（恢复中断） */
    sti();
}

void sched_tick(uint64_t now)
{
    struct per_cpu *pc = this_cpu();
    if (pc->current == NULL || pc->idle_task == NULL)
        return;

    /* 阶段二十：kill 请求（运行中的目标任务在下次 tick 自行退出） */
    if (pc->current->kill_pending) {
        pc->current->kill_pending = 0;
        task_exit(-1);
        return;                 /* task_exit 不返回 */
    }

    /* 唤醒本 CPU 睡眠队列到期任务 */
    wake_sleepers(pc, now);

    /* 累计运行时间（ps/top 显示） */
    if (pc->current->state != TASK_ZOMBIE)
        pc->current->total_ticks++;

    /* 时间片递减：可运行（非 BLOCKED/ZOMBIE）的当前任务耗尽置 need_resched。
     * 注意：空闲任务 state 为 TASK_READY（避免 schedule 重复入队），
     * 但必须计入递减，否则纯靠抢占的 AP 永远无法调度队列中的任务。 */
    if (pc->current->state != TASK_BLOCKED &&
        pc->current->state != TASK_ZOMBIE &&
        pc->current->time_slice > 0) {
        pc->current->time_slice--;
        if (pc->current->time_slice == 0)
            pc->need_resched = true;
    }

    if (pc->need_resched) {
        pc->need_resched = false;
        schedule();                     /* 抢占：中断上下文内切换 */
    }
}

void sleep(uint64_t ms)
{
    struct per_cpu *pc = this_cpu();
    struct task *t = pc->current;
    if (t == NULL)
        return;
    t->state = TASK_BLOCKED;
    t->sleep_until = timer_get_ticks() + ms;
    t->sleep_next = pc->sleep_head;
    pc->sleep_head = t;
    schedule();                         /* 切走；到期唤醒后从此返回 */
}

void yield(void)
{
    schedule();
}

void task_exit(int code)
{
    struct per_cpu *pc = this_cpu();
    struct task *t = pc->current;
    t->exit_code = code;
    t->state = TASK_ZOMBIE;
    uart_printf("[Nova] task %llu exited (code %d)\r\n",
                (unsigned long long)t->pid, code);

    /* 切走；当前任务不再回到就绪队列 */
    schedule();

    assert(!"task_exit: should never resume");
    for (;;)
        __asm__ volatile("hlt");
}

struct task *current_task(void)
{
    return this_cpu()->current;
}
