/* kernel/gdt/gdt_flush.S - Nova OS 阶段三：GDT 加载与段寄存器刷新
 *
 * gdt_flush(struct gdt_ptr *gdtr)：
 *   1. lgdt 加载新 GDT
 *   2. 用内核数据段选择子刷新 DS/ES/SS/FS/GS
 *   3. 通过远返回（lretq）切换到内核代码段（CS=0x08）
 *
 * 注意：切换 CS 前栈指针不变；长模式下段基址被忽略，栈地址不受影响。
 */

.section .text
.code64

.global gdt_flush
.type gdt_flush, @function
gdt_flush:
    /* 入参 RDI = &gdtr */
    lgdt (%rdi)

    /* 刷新数据段寄存器：GDT_KERNEL_DATA = 0x10 */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    /* 远返回切换 CS：压入新 CS 与返回地址后 lretq */
    pushq $0x08            /* GDT_KERNEL_CODE */
    leaq 1f(%rip), %rax
    pushq %rax
    lretq
1:
    ret
.size gdt_flush, . - gdt_flush
