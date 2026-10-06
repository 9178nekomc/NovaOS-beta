/* kernel/lapic/lapic.c - Nova OS 阶段十一：本地 APIC 实现
 *
 * LAPIC 通过 MMIO（基址 0xFEE00000，物理）访问，本阶段内核页表
 * 已恒等映射 [0,4GB)，直接以 0xFEE00000 访问即可。
 *
 * LAPIC 定时器频率校准（BSP）：用 PIT（已知 1000Hz）作参照——
 * 一次性模式、最大计数，测量 50ms 内消耗的计数，推出 1000Hz 的
 * 周期模式初始计数。AP 直接沿用该值。
 */
#include <stdbool.h>
#include <stdint.h>

#include "lapic.h"
#include "../../lib/io.h"
#include "../../lib/uart.h"
#include "../timer/timer.h"

#define LAPIC_BASE          0xFEE00000ull
#define LAPIC_REG(off)      (*(volatile uint32_t *)(uintptr_t)(LAPIC_BASE + (off)))

#define LAPIC_ID_REG        0x020u
#define LAPIC_SVR           0x0F0u
#define LAPIC_EOI           0x0B0u
#define LAPIC_LVT_TIMER     0x320u
#define LAPIC_TIMER_DIV     0x3E0u
#define LAPIC_TIMER_INITCNT 0x380u
#define LAPIC_TIMER_CURCNT  0x390u

#define LAPIC_SVR_ENABLE    0x100u
#define LAPIC_SPURIOUS_VEC  0xFFu
#define LAPIC_TIMER_VECTOR  0x20u
#define LAPIC_TIMER_PERIODIC (1u << 17)
#define LAPIC_TIMER_MASKED  (1u << 16)
#define LAPIC_TIMER_DIV16   0x3u      /* 分频 16 */

#define MSR_APIC_BASE       0x1Bu
#define MSR_APIC_BASE_ENABLE (1ull << 11)
#define MSR_APIC_BASE_X2    (1ull << 10)

static uint32_t lapic_timer_count = 62500;   /* 校准前默认（QEMU 1GHz 总线） */

static void lapic_enable(void)
{
    /* 使能 LAPIC，并强制 MMIO 模式（本阶段不使用 x2APIC） */
    uint64_t v = rdmsr(MSR_APIC_BASE);
    v |= MSR_APIC_BASE_ENABLE;
    v &= ~MSR_APIC_BASE_X2;
    wrmsr(MSR_APIC_BASE, v);

    /* SVR：APIC 使能 + 伪中断向量 0xFF */
    LAPIC_REG(LAPIC_SVR) = LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VEC;
}

/* 用 PIT（1000Hz）校准 LAPIC 定时器频率（一次性模式 + 最大计数） */
static void lapic_calibrate(void)
{
    LAPIC_REG(LAPIC_LVT_TIMER) = LAPIC_TIMER_MASKED | LAPIC_TIMER_VECTOR;
    LAPIC_REG(LAPIC_TIMER_DIV) = LAPIC_TIMER_DIV16;
    LAPIC_REG(LAPIC_TIMER_INITCNT) = 0xFFFFFFFFu;

    uint64_t t0 = timer_get_ticks();
    while (timer_get_ticks() < t0 + 50)
        __asm__ volatile("pause");

    uint32_t cur = LAPIC_REG(LAPIC_TIMER_CURCNT);
    uint32_t elapsed = 0xFFFFFFFFu - cur;    /* 50ms 消耗的计数 */
    lapic_timer_count = elapsed / 50;        /* 每 1ms 的计数 = 1000Hz 计数 */
    if (lapic_timer_count == 0)
        lapic_timer_count = 1;

    uart_printf("[Nova] LAPIC: calibrated %u counts/ms (timer count=%u)\r\n",
                (unsigned)lapic_timer_count, (unsigned)lapic_timer_count);
}

void lapic_init(bool calibrate)
{
    lapic_enable();
    if (calibrate)
        lapic_calibrate();

    /* 周期模式，向量 0x20，初始计数 = 校准值（AP 沿用 BSP 值） */
    LAPIC_REG(LAPIC_LVT_TIMER) = LAPIC_TIMER_PERIODIC | LAPIC_TIMER_VECTOR;
    LAPIC_REG(LAPIC_TIMER_DIV) = LAPIC_TIMER_DIV16;
    LAPIC_REG(LAPIC_TIMER_INITCNT) = lapic_timer_count;
}

void lapic_eoi(void)
{
    LAPIC_REG(LAPIC_EOI) = 0;
}

uint32_t lapic_get_id(void)
{
    return LAPIC_REG(LAPIC_ID_REG) >> 24;
}
