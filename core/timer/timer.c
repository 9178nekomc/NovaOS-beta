/* kernel/timer/timer.c - Nova OS 阶段四增强 + 阶段十一：系统时钟实现
 *
 * 8253/8254 PIT 通道 0（阶段四起，仅 BSP 可用）：
 *   端口 0x40（数据）/ 0x43（命令）
 *   命令字 0x36 = 通道 0 | 先低后高字节 | 模式 3（方波）| 二进制
 *   除数 = 1193182 / 1000 ≈ 1193 -> 1000.15Hz
 *
 * 阶段十一（SMP）：系统时钟切换到 LAPIC 定时器（每 CPU 一个）。
 * IRQ0（向量 0x20）处理函数同时兼容两种来源：
 *   - 切换前：PIT IRQ0（需 PIC EOI）
 *   - 切换后：LAPIC 定时器（需 LAPIC EOI）
 * 二者 EOI 对非本来源均为无害空操作，因此统一“双 EOI”。
 * tick 计数按 CPU 隔离（per_cpu->ticks）。
 */
#include <stdint.h>

#include "timer.h"
#include "../idt/idt.h"
#include "../../lib/io.h"
#include "../../lib/uart.h"
#include "../pic/pic.h"
#include "../sched/sched.h"
#include "../lapic/lapic.h"
#include "../smp/per_cpu.h"

#define PIT_CMD_PORT    0x43
#define PIT_DATA0_PORT  0x40
#define PIT_BASE_FREQ   1193182u
#define PIT_TARGET_HZ   1000u
#define TIMER_VECTOR    0x20

static void irq0_handler(struct isr_frame *frame)
{
    (void)frame;
    struct per_cpu *pc = this_cpu();
    pc->ticks++;
    pic_send_eoi(0);        /* 若来自 PIT（BSP 切换前）；否则空操作 */
    lapic_eoi();            /* 若来自 LAPIC 定时器；否则空操作 */
    sched_tick(pc->ticks);  /* 每-CPU 时间片调度钩子 */
}

int timer_init(void)
{
    uint32_t divisor = PIT_BASE_FREQ / PIT_TARGET_HZ;   /* 1193 */

    outb(PIT_CMD_PORT, 0x36);
    outb(PIT_DATA0_PORT, (uint8_t)(divisor & 0xFF));
    outb(PIT_DATA0_PORT, (uint8_t)((divisor >> 8) & 0xFF));

    idt_install_vector(TIMER_VECTOR, irq0_handler);
    pic_unmask_irq(0);
    return 0;
}

uint64_t timer_get_ticks(void)
{
    return this_cpu()->ticks;
}
