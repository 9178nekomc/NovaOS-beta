/* kernel/gdt/tss.h - Nova OS 阶段十二：任务状态段接口
 *
 * TSS 在本阶段唯一用途：ring3 -> ring0 特权级切换时，
 * CPU 从 TSS.RSP0 加载内核栈（INT 0x80 系统调用入口）。
 * 每任务内核栈不同，调度切换任务时由 schedule() 更新 RSP0。
 */
#ifndef NOVA_TSS_H
#define NOVA_TSS_H

#include <stdint.h>

/* 64 位 TSS 布局（IA-32e） */
struct tss {
    uint32_t reserved0;
    uint64_t rsp0;          /* 特权级 0 栈指针（ring3->ring0 使用） */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint32_t reserved2;
    uint32_t reserved3;
    uint16_t reserved4;
    uint16_t iopb;          /* I/O 位图基址（0 = 无位图） */
} __attribute__((packed));

/* TSS 选择子（GDT 索引 5） */
#define GDT_TSS_SEL 0x28u

/* 返回 TSS 指针（供 gdt_init 填描述符） */
struct tss *tss_get(void);

/* 初始化 TSS（清零、默认 RSP0 = 启动栈顶）并加载 TR */
void tss_init(void);

/* 更新 TSS.RSP0（调度器切换任务时调用） */
void tss_set_rsp0(uint64_t rsp0);

#endif /* NOVA_TSS_H */
