/* kernel/smp/per_cpu.h - Nova OS 阶段十一：每-CPU 控制块
 *
 * SMP 下每个 CPU 一个 struct per_cpu，挂在静态数组 per_cpus[] 中。
 * 调度器、LAPIC 定时器 tick、睡眠/就绪队列全部按 CPU 隔离：
 *   - current / idle_task / ready / sleep / need_resched（阶段十一每-CPU 调度）
 *   - ticks（本 CPU 的 LAPIC 定时器计数）
 *   - queue_lock（跨 CPU 入队互斥：BSP create_task_on 向 AP 队列入队时使用）
 *
 * this_cpu()：读取本 CPU 的 LAPIC ID 寄存器（MMIO 0xFEE00020，恒等映射），
 * 在 per_cpus[] 中按 lapic_id 查找自身槽位。该方式不依赖 GS 段基址
 * （QEMU TCG 下 WRMSR(IA32_GS_BASE) 后的 gs: 寻址并不可靠），
 * 对任意 LAPIC ID 分布均正确。
 */
#ifndef NOVA_PER_CPU_H
#define NOVA_PER_CPU_H

#include <stdbool.h>
#include <stdint.h>

#include "../../lib/spinlock.h"

struct task;

#define MAX_CPUS 16

/* LAPIC ID 寄存器（MMIO，物理恒等映射） */
#define NOVA_LAPIC_ID_REG (*(volatile uint32_t *)(uintptr_t)0xFEE00020u)

struct per_cpu {
    struct per_cpu *self;        /* 指向自身（冗余，便于调试/后续 GS 方案） */
    uint32_t cpu_id;             /* 逻辑 CPU 编号（0 = BSP） */
    uint32_t lapic_id;           /* 本 CPU 的 LAPIC ID */
    bool online;                 /* 已启动并完成初始化 */
    struct task *current;        /* 本 CPU 当前任务 */
    struct task *idle_task;      /* 本 CPU 空闲任务 */
    struct task *ready_head;     /* 本 CPU 就绪队列 */
    struct task *ready_tail;
    struct task *sleep_head;     /* 本 CPU 睡眠队列 */
    bool need_resched;
    uint64_t ticks;              /* 本 CPU tick 计数（LAPIC 定时器） */
    spinlock_t queue_lock;       /* 就绪/睡眠队列互斥（跨 CPU enqueue） */
};

extern struct per_cpu per_cpus[MAX_CPUS];
extern uint32_t cpu_count;       /* 已启动 CPU 数（含 BSP） */

/* 当前 CPU 的 per_cpu 控制块（按 LAPIC ID 查找） */
static inline struct per_cpu *this_cpu(void)
{
    uint32_t lapic_id = NOVA_LAPIC_ID_REG >> 24;
    for (uint32_t i = 0; i < cpu_count; i++) {
        if (per_cpus[i].lapic_id == lapic_id)
            return &per_cpus[i];
    }
    return &per_cpus[0];         /* 兜底：理论不可达 */
}

#endif /* NOVA_PER_CPU_H */

