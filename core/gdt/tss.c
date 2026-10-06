/* kernel/gdt/tss.c - Nova OS 阶段十二：任务状态段实现
 *
 * 单 TSS（阶段十二仅 BSP 运行用户任务）。tss_init 加载 TR，
 * tss_set_rsp0 由调度器在任务切换时更新（每任务内核栈不同）。
 */
#include <stdint.h>
#include <string.h>

#include "tss.h"

/* 启动栈顶（boot.S .bss 中定义，idle 任务默认 RSP0） */
extern uint64_t stack_top[];

static struct tss tss;

struct tss *tss_get(void)
{
    return &tss;
}

void tss_init(void)
{
    memset(&tss, 0, sizeof(tss));
    tss.iopb = (uint16_t)sizeof(tss);     /* I/O 位图紧接 TSS，0 = 无 */
    tss.rsp0 = (uint64_t)(uintptr_t)stack_top;

    __asm__ volatile("ltr %%ax" : : "a"(GDT_TSS_SEL));
}

void tss_set_rsp0(uint64_t rsp0)
{
    tss.rsp0 = rsp0;
}
