/* kernel/idt/idt.h - Nova OS 阶段三：IDT 模块接口
 *
 * 256 个中断门。中断向量编号约定：
 *   0-31    CPU 异常
 *   32-255  硬件中断（阶段四 PIC 重映射到 32-47）与软件中断（0x80 等）
 *
 * 中断处理流程：
 *   isrN (汇编 stub) -> common_isr_entry (pushaq) -> interrupt_dispatch (C)
 *   -> 自定义 handler -> popaq -> iretq
 */
#ifndef NOVA_IDT_H
#define NOVA_IDT_H

#include <stdint.h>

/* 中断门属性：P=1, DPL=0, 64 位中断门 */
#define IDT_ATTR_INTERRUPT_GATE 0x8Eu

/* C 处理函数收到的完整寄存器帧（与 isr.S 的 pushaq 布局一致） */
struct isr_frame {
    /* pushaq 压栈（低地址 -> 高地址） */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    /* stub 压入 */
    uint64_t vector;
    uint64_t error_code;
    /* CPU 压入 */
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed));

typedef void (*isr_handler_t)(struct isr_frame *frame);

/*
 * 初始化 IDT：全部 256 项指向 isr.S 的默认 stub，
 * 并设置处理器表寄存器（lidt）。返回 0 成功。
 */
int idt_init(void);

/*
 * 为指定向量安装自定义处理函数。
 * @vec     0-255
 * @handler 处理函数；传 NULL 恢复默认 stub（打印 "Unhandled interrupt N"）。
 * 返回 0 成功；失败返回负 errno。
 */
int idt_install_vector(int vec, isr_handler_t handler);

/* 安装 DPL=3 中断门（允许 ring3 触发；如 INT 0x80 系统调用） */
int idt_install_user_vector(int vec, isr_handler_t handler);

/* 重载当前 IDT（AP 启动时使用） */
void idt_reload(void);

/*
 * C 中断分发入口（isr.S 的 common_isr_entry 调用）：
 *   1. 有自定义 handler 则调用
 *   2. 否则打印 "Unhandled interrupt N" 与寄存器转储
 *   3. 异常（vec<32）打印后停机，避免 iretq 回到故障指令死循环
 */
void interrupt_dispatch(struct isr_frame *frame);

/* 汇编 stub 地址表（isr.S），isr_stub_table[vec] 为 isrN 入口地址 */
extern uint64_t isr_stub_table[];

#endif /* NOVA_IDT_H */
