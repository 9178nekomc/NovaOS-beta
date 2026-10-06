/* kernel/sched/sched.h - Nova OS 阶段九/十：任务管理与调度接口（单核）
 *
 * 协作式 + 抢占式：任务通过 yield()/sleep() 主动让出 CPU，
 * 同时 PIT 时钟（IRQ0）每 tick 递减当前任务时间片，耗尽后
 * 在中断上下文抢占切换到就绪队列队首（10ms 时间片轮转）。
 * 上下文切换：switch_to 汇编用 pushfq/pushaq 将 RFLAGS 与全部
 * GPR 压/弹任务栈，任务结构中的 regs.rsp 记录栈顶；新任务初始
 * 栈预置 RFLAGS=0x202、entry 与退出蹦床。
 */
#ifndef NOVA_SCHED_H
#define NOVA_SCHED_H

#include <stdint.h>

/* 任务栈大小：64KB（阶段二十：FreeType 字形渲染的 autofit 栈帧
 * 需 ~16KB，且渲染调用链叠加后 4KB 原栈会溢出写坏内存） */
#define TASK_STACK_SIZE 65536

/* 默认时间片：10ms（PIT 1000Hz = 10 tick） */
#define TASK_TIME_SLICE 10

enum task_state {
    TASK_RUNNING = 0,
    TASK_READY   = 1,
    TASK_BLOCKED = 2,
    TASK_ZOMBIE  = 3,
};

/* 保存的寄存器现场（字段顺序固定：sched.S 依赖 regs.rsp 偏移 128） */
struct regs {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rsp, rflags;
};

struct mm_struct;   /* 独立地址空间（后续阶段实现；当前共用内核页表） */

struct task {
    uint64_t pid;
    int state;
    struct regs regs;
    uint64_t kernel_stack;   /* 栈底（分配地址）；idle 为 0 = 借用启动栈 */
    struct mm_struct *mm;    /* NULL = 内核页表 */
    int exit_code;
    uint64_t time_slice;     /* 剩余时间片（tick） */
    uint64_t sleep_until;    /* sleep 唤醒 deadline（tick） */
    struct task *next;       /* 就绪队列链接 */
    struct task *sleep_next; /* 睡眠队列链接 */
    uint32_t cpu;            /* 绑定 CPU（阶段十一：任务运行在哪个 CPU） */
    uint64_t user_entry;     /* 用户态入口（阶段十二；0 = 纯内核任务） */
    uint64_t user_stack_top; /* 用户栈顶（阶段十二） */
    char name[16];           /* 阶段二十：任务名（ps/pidof 用） */
    struct task *all_next;   /* 阶段二十：全局任务注册表链接 */
    uint64_t total_ticks;    /* 阶段二十：累计运行 tick 数 */
    volatile int kill_pending; /* 阶段二十：被请求终止（sched_tick 检查） */
};

/*
 * 初始化调度器：创建 idle 任务（PID 0，借用启动栈）。
 * 返回 0 成功。
 */
int task_init(void);

/*
 * 创建任务：分配 task 结构与 4KB 内核栈，初始栈帧预置 entry 与
 * 退出蹦床，加入指定 CPU 的就绪队列（PID 从 1 递增）。
 * @cpu 目标 CPU（0 = BSP）；任务将只在目标 CPU 上被调度。
 */
struct task *create_task_on(uint32_t cpu, void (*entry)(void));

/* 创建任务并绑定到 BSP（CPU 0） */
struct task *create_task(void (*entry)(void));

/* 创建带名称的任务（ps/pidof 显示）；name 拷贝入 task */
struct task *create_named_task(const char *name, void (*entry)(void));

/* 设置任务名（截断到 15 字符） */
void task_set_name(struct task *t, const char *name);

/* ---- 阶段二十：任务枚举与终止（供 Shell ps/kill/pidof） ---- */

/* 全局注册的任务数（含 ZOMBIE；任务结构常驻不释放，指针稳定） */
uint32_t sched_count(void);

/* 按注册顺序取任务（i < sched_count()）；只读使用 */
struct task *sched_get(uint32_t i);

/*
 * 终止指定 PID 的任务：
 *   - 就绪/睡眠队列中的任务直接摘除（置 ZOMBIE）
 *   - 运行中的任务置 kill_pending，下次 tick 退出
 *   - 不允许杀 idle（PID 0）与当前 Shell 任务
 * 返回 0 成功；-ENOENT 无此任务；-EPERM 不允许。
 */
int sched_kill(uint64_t pid);

/*
 * 创建用户态任务（阶段十二）：分配内核栈与 task，初始帧入口为
 * user_enter（内核），首次调度时 iretq 进入 ring3。
 * @user_entry      用户程序入口虚拟地址（如 0x400000）
 * @user_stack_top  用户栈顶（向低地址增长）
 */
struct task *create_user_task(uint64_t user_entry, uint64_t user_stack_top);

/* 主动让出 CPU（进入 schedule） */
void yield(void);

/* 轮转调度：当前任务放回队尾，取队首任务切换（重置新任务时间片） */
void schedule(void);

/*
 * 时钟 tick 钩子（由 PIT IRQ0 处理函数每 1ms 调用）：
 *   - 唤醒睡眠队列到期任务
 *   - 递减当前任务时间片，耗尽置 need_resched 并抢占（schedule）
 * @now 当前 tick 计数
 */
void sched_tick(uint64_t now);

/*
 * 睡眠 ms 毫秒：置 BLOCKED 并加入睡眠队列，让出 CPU；
 * 到期后由时钟 tick 唤醒并重新进入就绪队列。
 */
void sleep(uint64_t ms);

/* 任务退出（entry 返回后由蹦床调用；不返回） */
void task_exit(int code);

/* 当前任务 */
struct task *current_task(void);

/* 汇编：保存 prev 上下文到 prev->regs，恢复 next->regs
 * （pushfq/pushaq + popaq/popfq；RFLAGS 随任务保存恢复） */
void switch_to(struct regs *prev_regs, struct regs *next_regs);

/* 任务首次运行蹦床（sched.S；entry 返回后进入，调用 task_exit） */
void task_trampoline(void);

#endif /* NOVA_SCHED_H */
