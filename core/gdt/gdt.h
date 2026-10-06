/* kernel/gdt/gdt.h - Nova OS 阶段三：GDT 模块接口
 *
 * 64 位全局描述符表：
 *   0x00 null       - 必须全零
 *   0x08 内核代码段 - ring0, L=1 (64 位)
 *   0x10 内核数据段 - ring0
 *   0x18 用户代码段 - ring3, L=1
 *   0x20 用户数据段 - ring3
 *
 * 段选择子常量（RPL=0；用户态使用时 RPL=3 即 +3）。
 */
#ifndef NOVA_GDT_H
#define NOVA_GDT_H

#include <stdint.h>

#define GDT_NULL_SEL      0x00
#define GDT_KERNEL_CODE   0x08
#define GDT_KERNEL_DATA   0x10
#define GDT_USER_CODE     0x18
#define GDT_USER_DATA     0x20

#define GDT_USER_CODE_RPL3  (GDT_USER_CODE | 3)
#define GDT_USER_DATA_RPL3  (GDT_USER_DATA | 3)

/* GDTR 结构（lgdt 用，packed） */
struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/*
 * 构建并加载 GDT（lgdt），随后重载 DS/ES/SS/FS/GS 并通过远返回切换 CS。
 * 返回 0 成功；失败返回负 errno。
 */
int gdt_init(void);

/*
 * 汇编函数（gdt.S）：加载 GDTR 并刷新段寄存器。
 * @gdtr 指向 struct gdt_ptr。
 */
void gdt_flush(struct gdt_ptr *gdtr);

/*
 * 重载当前 GDT（AP 启动时使用；会重载 GS 选择子，因此 GS.base
 * 必须在 gdt_reload 之后设置）。
 */
void gdt_reload(void);

#endif /* NOVA_GDT_H */
