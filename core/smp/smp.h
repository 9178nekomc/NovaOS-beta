/* kernel/smp/smp.h - Nova OS 阶段十一：SMP 多核启动接口 */
#ifndef NOVA_SMP_H
#define NOVA_SMP_H

#include <stdint.h>

/*
 * BSP 每-CPU 槽位初始化：per_cpus[0].self/cpu_id/online + 设置 BSP 的
 * GS 基址。必须在任何使用 this_cpu()/ticks/调度器 的代码之前调用
 * （且必须在 gdt_init 之后，因为 gdt_flush 会重载 GS 选择子）。
 */
void bsp_per_cpu_init(void);

/*
 * SMP 初始化：
 *   1. BSP：校准并启动 LAPIC 定时器（替换 PIT，屏蔽 IRQ0）
 *   2. 预构建各 AP 的空闲任务（含内核栈）
 *   3. 写入各 AP 的 goto_address，启动全部 AP
 * 返回 0 成功；失败返回负 errno（无响应/无 AP）。
 */
int smp_init(void);

/* 等待全部 AP 上线（带 2 秒超时）；返回实际上线 CPU 数 */
uint32_t smp_wait_online(void);

/* 当前已上线 CPU 数 */
uint32_t smp_online_count(void);

#endif /* NOVA_SMP_H */
