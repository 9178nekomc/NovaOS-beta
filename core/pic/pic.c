/* kernel/pic/pic.c - Nova OS 阶段四：8259A PIC 实现
 *
 * 初始化后 IRQ0-7 -> 中断 0x20-0x27，IRQ8-15 -> 0x28-0x2F，
 * 与 IDT 硬件中断区（>=0x20）对齐，避免与 CPU 异常（0-31）冲突。
 */
#include <stdint.h>

#include "pic.h"
#include "../../lib/io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT       0x11  /* 边沿触发 | 级联 | 需要 ICW4 */
#define ICW4_8086       0x01
#define PIC1_ICW2_VEC   0x20  /* IRQ0 -> int 0x20 */
#define PIC2_ICW2_VEC   0x28  /* IRQ8 -> int 0x28 */
#define PIC1_ICW3_CASCADE 0x04 /* 从片接主片 IRQ2 */
#define PIC2_ICW3_CASCADE 0x02
#define OCW2_EOI        0x20  /* 非特指 EOI */
#define OCW1_MASK_ALL   0xFF

void pic_init(void)
{
    /* 初始化序列：ICW1 -> ICW2 -> ICW3 -> ICW4 */
    outb(PIC1_CMD, ICW1_INIT);
    outb(PIC2_CMD, ICW1_INIT);
    outb(PIC1_DATA, PIC1_ICW2_VEC);
    outb(PIC2_DATA, PIC2_ICW2_VEC);
    outb(PIC1_DATA, PIC1_ICW3_CASCADE);
    outb(PIC2_DATA, PIC2_ICW3_CASCADE);
    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);

    /* 默认屏蔽全部 */
    outb(PIC1_DATA, OCW1_MASK_ALL);
    outb(PIC2_DATA, OCW1_MASK_ALL);
}

void pic_mask_irq(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t val = inb(port);
    val |= (uint8_t)(1u << (irq & 7));
    outb(port, val);
}

void pic_unmask_irq(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t val = inb(port);
    val &= (uint8_t)~(1u << (irq & 7));
    outb(port, val);
}

void pic_mask_all(void)
{
    outb(PIC1_DATA, OCW1_MASK_ALL);
    outb(PIC2_DATA, OCW1_MASK_ALL);
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, OCW2_EOI);   /* 从片 EOI */
    outb(PIC1_CMD, OCW2_EOI);       /* 主片 EOI */
}
