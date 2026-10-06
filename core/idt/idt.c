/* kernel/idt/idt.c - Nova OS 阶段三：IDT 实现
 *
 * 256 个中断门，全部指向 isr.S 生成的汇编 stub；
 * C 分发 interrupt_dispatch() 依据向量号路由到自定义 handler
 * 或默认 stub（打印 "Unhandled interrupt N" + 寄存器转储）。
 */
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "idt.h"
#include "../../lib/io.h"
#include "../../lib/uart.h"
#include "../../graphics/terminal/terminal.h"

/* 64 位 IDT 表项 */
struct idt_entry {
    uint16_t offset_low;   /* 处理函数低 16 位 */
    uint16_t selector;     /* 代码段选择子（内核代码段） */
    uint8_t  ist;          /* IST（本阶段不用，0） */
    uint8_t  type_attr;    /* 中断门属性 */
    uint16_t offset_mid;   /* 处理函数中 16 位 */
    uint32_t offset_high;  /* 处理函数高 32 位 */
    uint32_t reserved;     /* 保留 */
} __attribute__((packed));

/* IDTR 结构（lidt 用，packed） */
struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static isr_handler_t handlers[256];

/* 常用异常名（0-31），供默认 stub 输出 */
static const char *const exception_names[32] = {
    "Divide-by-zero",          "Debug",                "NMI",
    "Breakpoint",              "Overflow",             "Bound range exceeded",
    "Invalid opcode",          "Device not available", "Double fault",
    "Coprocessor segment overrun", "Invalid TSS",       "Segment not present",
    "Stack-segment fault",     "General protection",   "Page fault",
    "Reserved",                "x87 FP exception",     "Alignment check",
    "Machine check",           "SIMD exception",       "Virtualization exception",
    "Control protection",      "Reserved",             "Reserved",
    "Reserved",                "Reserved",             "Reserved",
    "Reserved",                "Reserved",             "VMM communication",
    "Security exception",      "Reserved",
};

static inline void set_idt_entry(int vec, uint64_t handler)
{
    idt[vec].offset_low = (uint16_t)(handler & 0xFFFF);
    idt[vec].selector = 0x08;                     /* GDT_KERNEL_CODE */
    idt[vec].ist = 0;
    idt[vec].type_attr = IDT_ATTR_INTERRUPT_GATE; /* P=1, DPL=0, 64 位中断门 */
    idt[vec].offset_mid = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[vec].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    idt[vec].reserved = 0;
}

/* ------------------------------------------------------------------ */
/* 默认 stub：打印 "Unhandled interrupt N" + 寄存器转储             */
/* ------------------------------------------------------------------ */

void interrupt_dispatch(struct isr_frame *frame)
{
    /* 优先调用自定义 handler */
    if (frame->vector < 256 && handlers[frame->vector] != NULL) {
        handlers[frame->vector](frame);
        return;
    }

    /* 默认 stub */
    if (frame->vector < 32) {
        /* 重入保护：异常处理内的终端渲染再次触发异常时（如终端/字体
         * 状态损坏），只打串口并停机，避免递归栈溢出（曾连续嵌套 4 次
         * 把 rsp 压到 stack_bottom 之下） */
        static volatile uint32_t panic_nesting;
        if (panic_nesting != 0) {
            uart_printf("[Nova] PANIC: nested exception %u, halting\r\n",
                        (unsigned)frame->vector);
            for (;;)
                __asm__ volatile("hlt");
        }
        panic_nesting = 1;

        /* 先串口后终端：若异常发生在终端渲染路径内，终端输出会再次
         * 触发异常（双/三重故障），串口先打保证故障信息不丢 */
        uint64_t cr2 = 0;
        if (frame->vector == 14)   /* #PF */
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        uart_printf("[Nova] PANIC: exception %u (%s) err=0x%x rip=%p "
                    "cr2=%p cs=0x%x rsp=%p ss=0x%x\n",
                    (unsigned)frame->vector,
                    exception_names[frame->vector],
                    (unsigned)frame->error_code, (void *)frame->rip,
                    (void *)cr2, (unsigned)frame->cs,
                    (void *)frame->rsp, (unsigned)frame->ss);
        uart_printf("[Nova] PANIC: rax=%p rbx=%p rcx=%p rdx=%p rsi=%p "
                    "rdi=%p rbp=%p\n",
                    (void *)frame->rax, (void *)frame->rbx,
                    (void *)frame->rcx, (void *)frame->rdx,
                    (void *)frame->rsi, (void *)frame->rdi,
                    (void *)frame->rbp);
        uart_printf("[Nova] PANIC: r8=%p r9=%p r10=%p r11=%p r12=%p "
                    "r13=%p r14=%p r15=%p rflags=%p\n",
                    (void *)frame->r8, (void *)frame->r9,
                    (void *)frame->r10, (void *)frame->r11,
                    (void *)frame->r12, (void *)frame->r13,
                    (void *)frame->r14, (void *)frame->r15,
                    (void *)frame->rflags);
        /* 栈顶 8 个字（rsp 起）：帮助定位被写坏的返回地址 */
        {
            uint64_t *sp = (uint64_t *)(uintptr_t)frame->rsp;
            uart_printf("[Nova] PANIC: stack:");
            for (int i = 0; i < 8; i++)
                uart_printf(" %p", (void *)*sp++);
            uart_printf("\n");
        }
        /* #PF：dump cr2 的页表遍历，判断映射缺失还是页表损坏 */
        if (frame->vector == 14 && cr2 != 0) {
            extern uint64_t vmm_walk(uint64_t virt);
            uint64_t phys = vmm_walk(cr2);
            uart_printf("[Nova] PANIC: walk(%p) -> phys=%p\n",
                        (void *)cr2, (void *)phys);
        }
        terminal_printf("\n[PANIC] Exception %u: %s\n",
                        (unsigned)frame->vector,
                        exception_names[frame->vector]);
        terminal_printf("  err=0x%x rip=%p cs=0x%x rflags=%p rsp=%p\n",
                        (unsigned)frame->error_code,
                        (void *)frame->rip, (unsigned)frame->cs,
                        (void *)frame->rflags, (void *)frame->rsp);
        terminal_printf("  rax=%p rbx=%p rcx=%p rdx=%p\n",
                        (void *)frame->rax, (void *)frame->rbx,
                        (void *)frame->rcx, (void *)frame->rdx);
        terminal_printf("  rsi=%p rdi=%p rbp=%p\n",
                        (void *)frame->rsi, (void *)frame->rdi,
                        (void *)frame->rbp);
        /* 异常返回原指令会再次触发，直接停机 */
        for (;;)
            __asm__ volatile("hlt");
    }

    /* 硬件/软件中断：打印后正常 iretq 返回 */
    terminal_printf("\n[ISR] Unhandled interrupt %u (err=0x%x rip=%p)\n",
                    (unsigned)frame->vector, (unsigned)frame->error_code,
                    (void *)frame->rip);
    uart_printf("[Nova] ISR: unhandled interrupt %u err=0x%x rip=%p\n",
                (unsigned)frame->vector, (unsigned)frame->error_code,
                (void *)frame->rip);
}

/* ------------------------------------------------------------------ */
/* 公开接口                                                        */
/* ------------------------------------------------------------------ */

int idt_init(void)
{
    memset(idt, 0, sizeof(idt));
    memset(handlers, 0, sizeof(handlers));

    for (int i = 0; i < 256; i++)
        set_idt_entry(i, isr_stub_table[i]);

    struct idt_ptr idtr;
    idtr.limit = (uint16_t)(sizeof(idt) - 1);
    idtr.base = (uint64_t)(uintptr_t)idt;

    __asm__ volatile("lidt %0" : : "m"(idtr) : "memory");
    return 0;
}

/* 阶段十一：AP 启动时重载同一 IDT */
void idt_reload(void)
{
    struct idt_ptr idtr;
    idtr.limit = (uint16_t)(sizeof(idt) - 1);
    idtr.base = (uint64_t)(uintptr_t)idt;
    __asm__ volatile("lidt %0" : : "m"(idtr) : "memory");
}

int idt_install_vector(int vec, isr_handler_t handler)
{
    if (vec < 0 || vec > 255)
        return -EINVAL;
    handlers[vec] = handler;
    return 0;
}

/* 阶段十二：安装 DPL=3 的中断门（允许 ring3 触发，如 INT 0x80） */
int idt_install_user_vector(int vec, isr_handler_t handler)
{
    if (vec < 0 || vec > 255)
        return -EINVAL;
    handlers[vec] = handler;
    idt[vec].type_attr = 0xEE;   /* P=1, DPL=3, 64 位中断门 */
    return 0;
}
