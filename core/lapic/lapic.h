/* kernel/lapic/lapic.h - Nova OS 阶段十一：本地 APIC 接口
 *
 * LAPIC 提供每 CPU 的中断控制：定时器、IPI、EOI。
 * 本阶段使用 LAPIC 定时器替换 PIT 作为系统时钟源（PIT 仅能送达 BSP）。
 */
#ifndef NOVA_LAPIC_H
#define NOVA_LAPIC_H

#include <stdbool.h>
#include <stdint.h>

/*
 * 初始化本地 APIC：
 *   1. 使能 LAPIC（IA32_APIC_BASE 置位，强制 MMIO 模式）
 *   2. 配置 SVR（伪中断向量 0xFF）
 *   3. 启动 LAPIC 定时器（周期模式，向量 0x20，~1000Hz）
 * @calibrate 仅 BSP 需要：切换系统时钟前用 PIT（已知 1000Hz）校准
 *            定时器计数；AP 沿用 BSP 校准值（传 false）。
 */
void lapic_init(bool calibrate);

/* 发送 LAPIC EOI（写 EOI 寄存器；对非 LAPIC 中断为无害空操作） */
void lapic_eoi(void);

/* 读取本 CPU 的 LAPIC ID */
uint32_t lapic_get_id(void);

#endif /* NOVA_LAPIC_H */
