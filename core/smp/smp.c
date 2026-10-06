/* kernel/smp/smp.c - Nova OS 阶段十一：SMP 多核启动实现
 *
 * 基于 Limine SMP 请求：
 *   - 请求存在即提示引导器启动应用处理器（AP）
 *   - 内核为每个 AP 的 limine_smp_info.goto_address 写入入口函数，
 *     该写入会使 AP 立即跳转到入口（RDI = info*，Limine 提供 64KiB 临时栈）
 *   - BSP 不走回调：在 kmain 中手动初始化
 *
 * 每-CPU 状态（per_cpu）：
 *   - 静态数组 per_cpus[MAX_CPUS]，GS.base 指向当前 CPU 的槽位
 *   - AP 空闲任务由 BSP 预构建（kmalloc），AP 启动后直接成为其 current
 *   - AP 初始化顺序：切 CR3 -> 重载 GDT/IDT -> 设 GS.base -> 置 current
 *     -> 启动 LAPIC 定时器 -> 首次切换进入空闲任务（Limine 临时栈作废）
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <limine.h>

#include "smp.h"
#include "per_cpu.h"
#include "../lapic/lapic.h"
#include "../gdt/gdt.h"
#include "../idt/idt.h"
#include "../../lib/io.h"
#include "../../lib/uart.h"
#include "../mm/vmm.h"
#include "../mm/kmalloc.h"
#include "../sched/sched.h"
#include "../pic/pic.h"
#include "../timer/timer.h"
#include "../types/assert.h"

/* Limine SMP 请求（必须位于 .requests 段） */
__attribute__((used, section(".requests")))
static volatile struct limine_smp_request smp_request = {
    .id = LIMINE_SMP_REQUEST,
    .revision = 0,
    .response = NULL,
    .flags = 0,      /* 不请求 x2APIC：本阶段使用 MMIO LAPIC */
};

struct per_cpu per_cpus[MAX_CPUS];
uint32_t cpu_count = 1;

/* AP 空闲任务（BSP 预构建，直接成为各 AP 的 current） */
static struct task *ap_idle_tasks[MAX_CPUS];
/* AP 引导上下文：首次切换的“前一个”伪上下文（切换后作废） */
static struct regs ap_boot_regs[MAX_CPUS];

/* 设置当前 CPU 的 GS 基址（保留备用；this_cpu() 目前用 LAPIC ID 查找，
 * 不依赖 GS。QEMU TCG 下 WRMSR(IA32_GS_BASE) 后的 gs: 寻址不可靠） */
static void set_gs_base(struct per_cpu *pc)
{
    wrmsr(0xC0000100ull, (uint64_t)(uintptr_t)pc);   /* IA32_GS_BASE */
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0) : "memory");
}

/* AP 空闲任务主循环：切到内核页表后永久 hlt（由 LAPIC 定时器唤醒） */
static void ap_idle_main(void)
{
    write_cr3(vmm_get_cr3());      /* 离开 Limine 页表，进入内核地址空间 */
    for (;;)
        __asm__ volatile("hlt");
}

/* 构建一个 AP 空闲任务（内核栈 + 初始帧；不加入就绪队列） */
static struct task *build_ap_idle(uint32_t cpu)
{
    struct task *t = kcalloc(1, sizeof(struct task));
    assert(t != NULL);
    uint8_t *stack = kmalloc(TASK_STACK_SIZE);
    assert(stack != NULL);

    t->pid = 0;                    /* 空闲任务 PID 0（与 BSP idle 一致） */
    t->state = TASK_READY;
    t->kernel_stack = (uint64_t)(uintptr_t)stack;
    t->mm = NULL;
    t->exit_code = 0;
    t->time_slice = TASK_TIME_SLICE;
    t->cpu = cpu;

    /* 初始栈帧：[15×0][rflags=0x202][entry=ap_idle_main][task_trampoline] */
    uint64_t *sp = (uint64_t *)(uintptr_t)(stack + TASK_STACK_SIZE);
    *--sp = (uint64_t)(uintptr_t)task_trampoline;
    *--sp = (uint64_t)(uintptr_t)ap_idle_main;
    *--sp = 0x202;                 /* IF=1（popfq） */
    for (int i = 0; i < 15; i++)
        *--sp = 0;

    t->regs.rsp = (uint64_t)sp;
    t->regs.rip = (uint64_t)(uintptr_t)ap_idle_main;
    t->regs.rflags = 0x202;
    return t;
}

/* ------------------------------------------------------------------ */
/* AP 入口（Limine 跳转目标；RDI = limine_smp_info *）              */
/* ------------------------------------------------------------------ */

/* 启用 SSE（与 boot.S 一致）：
 * Limine 启动的 AP 与 BSP 一样 SSE 处于关闭状态（CR4.OSFXSR=0），
 * 而 gcc -O2 会生成 SSE 指令，不启用会在首次 xmm 使用时 #UD。
 * 必须在 ap_entry 的任何 C 代码之前调用。 */
static void sse_enable(void)
{
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~0x4ull;                    /* 清 CR0.EM */
    cr0 |= 0x2ull;                     /* 置 CR0.MP */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 0x600ull;                   /* 置 CR4.OSFXSR | CR4.OSXMMEXCPT */
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

    uint32_t mxcsr = 0x1F80;
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr) : "memory");
}

static void ap_entry(struct limine_smp_info *info)
{
    uint32_t lapic_id;

    /* 必须先启用 SSE：后续 C 代码（含本函数其余部分）依赖 xmm */
    sse_enable();
    lapic_id = info->lapic_id;

    /* 依据 LAPIC ID 找到本 AP 的 per_cpu 槽位 */
    uint32_t i;
    for (i = 1; i < cpu_count; i++) {
        if (per_cpus[i].lapic_id == lapic_id)
            break;
    }
    if (i >= cpu_count) {
        for (;;)
            __asm__ volatile("hlt");     /* 槽位异常：挂起 */
    }
    struct per_cpu *pc = &per_cpus[i];

    /* 重载内核 GDT/IDT（GS 基址不依赖 gdt_reload，无顺序约束） */
    gdt_reload();
    idt_reload();

    pc->online = true;

    /* 先挂载 current/idle，再启动定时器：tick 一到即可安全调度 */
    pc->current = ap_idle_tasks[i];
    pc->idle_task = ap_idle_tasks[i];
    pc->need_resched = false;

    lapic_init(false);             /* 启动本 CPU LAPIC 定时器（沿用校准值） */

    uart_printf("[Nova] CPU %u online (LAPIC %u)\r\n",
                (unsigned)i, (unsigned)lapic_id);

    /* 首次切换进入空闲任务；Limine 临时栈作废，永不返回 */
    switch_to(&ap_boot_regs[i], &pc->idle_task->regs);
    for (;;)
        __asm__ volatile("hlt");
}

/* ------------------------------------------------------------------ */
/* BSP 侧接口                                                       */
/* ------------------------------------------------------------------ */

void bsp_per_cpu_init(void)
{
    per_cpus[0].self = &per_cpus[0];
    per_cpus[0].cpu_id = 0;
    per_cpus[0].lapic_id = lapic_get_id();   /* 真实 LAPIC ID（this_cpu 查找依据） */
    per_cpus[0].online = true;
    per_cpus[0].ticks = 0;
    per_cpus[0].queue_lock = (spinlock_t)SPINLOCK_INIT;
    set_gs_base(&per_cpus[0]);               /* 备用（不影响 this_cpu） */
}

int smp_init(void)
{
    if (smp_request.response == NULL || smp_request.response->cpu_count < 1)
        return -ENODEV;

    uint64_t total = smp_request.response->cpu_count;
    uint32_t bsp_lapic = smp_request.response->bsp_lapic_id;
    per_cpus[0].lapic_id = bsp_lapic;

    /* BSP：校准并启动 LAPIC 定时器，此后系统时钟为 LAPIC（屏蔽 PIT IRQ0） */
    lapic_init(true);
    pic_mask_irq(0);
    uart_printf("[Nova] LAPIC: BSP timer armed, PIT IRQ0 masked\r\n");

    /* 预构建 AP 空闲任务并分配槽位，随后一次性启动全部 AP */
    uint32_t n = 1;
    for (uint64_t j = 0; j < total && n < MAX_CPUS; j++) {
        struct limine_smp_info *inf = smp_request.response->cpus[j];
        if (inf->lapic_id == bsp_lapic)
            continue;
        per_cpus[n].self = &per_cpus[n];
        per_cpus[n].cpu_id = n;
        per_cpus[n].lapic_id = inf->lapic_id;
        per_cpus[n].online = false;
        per_cpus[n].ticks = 0;
        per_cpus[n].queue_lock = (spinlock_t)SPINLOCK_INIT;
        ap_idle_tasks[n] = build_ap_idle(n);
        inf->goto_address = ap_entry;      /* 原子写：该 AP 立即启动 */
        n++;
    }
    cpu_count = n;

    uart_printf("[Nova] SMP: %u CPUs requested (BSP LAPIC %u)\r\n",
                (unsigned)cpu_count, (unsigned)bsp_lapic);
    return 0;
}

uint32_t smp_online_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < cpu_count; i++) {
        if (per_cpus[i].online)
            n++;
    }
    return n;
}

uint32_t smp_wait_online(void)
{
    uint64_t deadline = timer_get_ticks() + 2000;   /* 2 秒超时 */
    while (smp_online_count() < cpu_count) {
        if (timer_get_ticks() > deadline)
            break;
        __asm__ volatile("hlt");
    }
    return smp_online_count();
}
