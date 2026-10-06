/* kernel/timer/timer.h - Nova OS 阶段四增强：系统时钟模块接口
 *
 * PIT 通道 0 以 1000Hz 方波产生 IRQ0（向量 0x20），中断中递增 tick 计数。
 * 本阶段用于驱动终端光标闪烁；阶段十在此基础上实现时间片抢占调度。
 */
#ifndef NOVA_TIMER_H
#define NOVA_TIMER_H

#include <stdint.h>

/*
 * 初始化 PIT 通道 0（1000Hz），安装 IRQ0 处理函数并取消屏蔽。
 * 返回 0 成功；失败返回负 errno。
 */
int timer_init(void);

/* 自初始化以来的 tick 数（1000Hz，即毫秒数） */
uint64_t timer_get_ticks(void);

#endif /* NOVA_TIMER_H */
