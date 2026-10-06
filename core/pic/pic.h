/* kernel/pic/pic.h - Nova OS 阶段四：8259A PIC 模块接口
 *
 * 约定：主片 IRQ0-7 映射到中断向量 0x20-0x27，
 *       从片 IRQ8-15 映射到 0x28-0x2F（经主片 IRQ2 级联）。
 */
#ifndef NOVA_PIC_H
#define NOVA_PIC_H

#include <stdint.h>

/*
 * 初始化 8259A（主片 0x20/0x21，从片 0xA0/0xA1）：
 *   ICW1=0x11（边沿触发 + 级联 + 需要 ICW4）
 *   ICW2：主片 0x20（IRQ0->int 0x20），从片 0x28（IRQ8->int 0x28）
 *   ICW3：主片 0x04（从片接 IRQ2），从片 0x02
 *   ICW4=0x01（8086 模式）
 *   OCW1=0xFF（屏蔽全部中断）
 */
void pic_init(void);

/* 屏蔽指定 IRQ（OCW1） */
void pic_mask_irq(uint8_t irq);

/* 取消屏蔽指定 IRQ */
void pic_unmask_irq(uint8_t irq);

/* 屏蔽全部 IRQ */
void pic_mask_all(void);

/*
 * 中断处理完成后发送 EOI（OCW2=0x20 非特指 EOI）。
 * IRQ>=8 时先给从片发 EOI。
 */
void pic_send_eoi(uint8_t irq);

#endif /* NOVA_PIC_H */
