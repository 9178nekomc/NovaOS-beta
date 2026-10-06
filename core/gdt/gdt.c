/* kernel/gdt/gdt.c - Nova OS 阶段三/十二：GDT 与 TSS 实现
 *
 * 64 位模式段描述符布局（8 字节）：
 *   limit_low(16) base_low(16) base_mid(8) access(8)
 *   limit_high_flags(8) base_high(8)
 *
 * 长模式下基址/限长被忽略（除 FS/GS），CS.L=1 表示 64 位代码。
 * 阶段十二：索引 5（0x28）为 64 位可用 TSS 描述符（16 字节，
 * 基址含高 32 位），供 ring3 进入 ring0 时切换内核栈（RSP0）。
 */
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "gdt.h"
#include "tss.h"

#define GDT_ENTRIES 5

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  limit_flags;
    uint8_t  base_high;
} __attribute__((packed));

/* 64 位系统描述符（TSS/门）：8 字节基础 + 基址高 32 位（共 16 字节） */
struct gdt_sys_entry64 {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  limit_flags;
    uint8_t  base_high;
    uint32_t base_high32;
    uint32_t reserved;
} __attribute__((packed));

/* 访问字节（access）位：P=0x80 DPL=0x60 S=0x10 E=0x08 W/R=0x02 */
#define ACCESS_PRESENT     0x80u
#define ACCESS_DPL0        0x00u
#define ACCESS_DPL3        0x60u
#define ACCESS_SYSTEM      0x10u        /* S=1：代码/数据段 */
#define ACCESS_CODE        0x08u
#define ACCESS_WRITABLE    0x02u        /* 数据段可写 / 代码段可读 */

/* 64 位可用 TSS 类型（S=0 系统段）：0x09 | P=0x80 -> 0x89 */
#define ACCESS_TSS64_AVAIL (0x09u | ACCESS_PRESENT)

/* limit_flags：L=0x20（64 位代码），G=0x80（4K 粒度，数据段惯例） */
#define FLAG_LONGMODE      0x20u
#define FLAG_GRANULARITY   0x80u

/* 经典 5 段 + 紧随其后的 16 字节 TSS 描述符（0x28），共 48 字节。
 * 必须为单个连续对象，保证 GDTR base 处 0x28 偏移就是 TSS 描述符。 */
struct gdt_table_t {
    struct gdt_entry entries[5];
    struct gdt_sys_entry64 tss;
} __attribute__((packed));

static struct gdt_table_t gdt_table;

#define gdt        (gdt_table.entries)
#define gdt_tss    (gdt_table.tss)

static void set_gdt_entry(size_t idx, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t flags)
{
    gdt[idx].limit_low = (uint16_t)(limit & 0xFFFF);
    gdt[idx].base_low = (uint16_t)(base & 0xFFFF);
    gdt[idx].base_mid = (uint8_t)((base >> 16) & 0xFF);
    gdt[idx].access = access;
    gdt[idx].limit_flags = (uint8_t)(((limit >> 16) & 0x0F) | (flags & 0xF0));
    gdt[idx].base_high = (uint8_t)((base >> 24) & 0xFF);
}

/* 设置 64 位系统描述符（TSS）：基址为完整 64 位内核虚拟地址 */
static void set_gdt_sys64(struct gdt_sys_entry64 *e, uint64_t base,
                          uint32_t limit, uint8_t access)
{
    e->limit_low = (uint16_t)(limit & 0xFFFF);
    e->base_low = (uint16_t)(base & 0xFFFF);
    e->base_mid = (uint8_t)((base >> 16) & 0xFF);
    e->access = access;
    e->limit_flags = (uint8_t)((limit >> 16) & 0x0F);
    e->base_high = (uint8_t)((base >> 24) & 0xFF);
    e->base_high32 = (uint32_t)(base >> 32);
    e->reserved = 0;
}

int gdt_init(void)
{
    memset(&gdt_table, 0, sizeof(gdt_table));

    /* 0x00: null 描述符（已清零） */

    /* 0x08: 内核代码段 - ring0 64 位 */
    set_gdt_entry(1, 0, 0,
                  ACCESS_PRESENT | ACCESS_DPL0 | ACCESS_SYSTEM | ACCESS_CODE | ACCESS_WRITABLE,
                  FLAG_LONGMODE);

    /* 0x10: 内核数据段 - ring0 */
    set_gdt_entry(2, 0, 0xFFFFF,
                  ACCESS_PRESENT | ACCESS_DPL0 | ACCESS_SYSTEM | ACCESS_WRITABLE,
                  FLAG_GRANULARITY);

    /* 0x18: 用户代码段 - ring3 64 位 */
    set_gdt_entry(3, 0, 0,
                  ACCESS_PRESENT | ACCESS_DPL3 | ACCESS_SYSTEM | ACCESS_CODE | ACCESS_WRITABLE,
                  FLAG_LONGMODE);

    /* 0x20: 用户数据段 - ring3 */
    set_gdt_entry(4, 0, 0xFFFFF,
                  ACCESS_PRESENT | ACCESS_DPL3 | ACCESS_SYSTEM | ACCESS_WRITABLE,
                  FLAG_GRANULARITY);

    /* 0x28: 64 位可用 TSS（阶段十二，ring3->ring0 内核栈切换） */
    set_gdt_sys64(&gdt_table.tss, (uint64_t)(uintptr_t)tss_get(),
                  (uint32_t)(sizeof(struct tss) - 1), ACCESS_TSS64_AVAIL);

    struct gdt_ptr gdtr;
    gdtr.limit = (uint16_t)(sizeof(gdt_table) - 1);
    gdtr.base = (uint64_t)(uintptr_t)&gdt_table;

    gdt_flush(&gdtr);
    return 0;
}

/* 阶段十一：AP 启动时重载同一 GDT（注意：会重载 GS 选择子，
 * 因此 GS.base（每-CPU 指针）必须在 gdt_reload 之后设置） */
void gdt_reload(void)
{
    struct gdt_ptr gdtr;
    gdtr.limit = (uint16_t)(sizeof(gdt_table) - 1);
    gdtr.base = (uint64_t)(uintptr_t)&gdt_table;
    gdt_flush(&gdtr);
}
