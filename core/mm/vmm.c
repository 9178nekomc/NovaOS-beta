/* kernel/mm/vmm.c - Nova OS 阶段七：四级分页与虚拟内存实现
 *
 * 页表访问：所有页表页由 buddy 分配于低位物理内存，通过 identity 映射
 * （物理地址当虚拟地址）读写；identity 映射在 vmm_init 中先行建立，
 * 因此切换 CR3 前后均可访问。
 *
 * 布局：
 *   PML4[0]   -> PDP0: identity [0, 4GB) 2MB 大页
 *   PML4[256] -> PDP256: HHDM [0xffff800000000000, +4GB) 2MB 大页
 *   PML4[511] -> PDP511: 内核高半区（4KB 页）及其余映射
 *
 * 切换安全：新页表覆盖内核代码/栈（高半区）、PMM 位图与帧缓冲（HHDM）、
 * 伙伴空闲链表（identity），CR3 切换瞬间无缝。
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "vmm.h"
#include "../../lib/io.h"

/* 阶段十二：用户区布局（2MB 对齐，恒等映射的空闲物理区） */
#define NOVA_USER_CODE_BASE  0x400000ull   /* 用户代码：0x400000 */
#define NOVA_USER_STACK_BASE 0x7E00000ull  /* 用户栈：0x7E00000（2MB） */
#include "buddy.h"
#include "pmm.h"
#include "../types/assert.h"
#include "../../lib/io.h"
#include "../../lib/uart.h"
#include "../../graphics/terminal/terminal.h"

/* ------------------------------------------------------------------ */
/* 页表项（64 位 = 8 字节，packed；位域必须凑满 64 位！）            */
/* ------------------------------------------------------------------ */
struct page_entry {
    uint64_t present   : 1;   /* P    bit 0  */
    uint64_t rw        : 1;   /* RW   bit 1  */
    uint64_t us        : 1;   /* US   bit 2  */
    uint64_t pwt       : 1;   /* PWT  bit 3  */
    uint64_t pcd       : 1;   /* PCD  bit 4  */
    uint64_t accessed  : 1;   /* A    bit 5  */
    uint64_t dirty     : 1;   /* D    bit 6  */
    uint64_t ps_pat    : 1;   /* PS/PAT bit 7 */
    uint64_t global    : 1;   /* G    bit 8  */
    uint64_t avail     : 3;   /* AVL  bits 9-11 */
    uint64_t address   : 40;  /* 物理地址  bits 12-51 */
    uint64_t reserved  : 11;  /* 保留    bits 52-62（必须占位！） */
    uint64_t nx        : 1;   /* NX    bit 63 */
} __attribute__((packed));

/* 页表项必须恰好 8 字节，否则 parent[index] 寻址错位（实测 sizeof=7 时
 * 只有第 0 项正确，其余项写入错误偏移导致 CR3 切换后取指 #PF） */
_Static_assert(sizeof(struct page_entry) == 8,
               "page table entry must be 8 bytes");

static uint64_t pml4_phys;
static uint64_t hhdm_off;
static uint64_t kernel_phys_base_g;   /* 内核镜像物理基址（重建映射用） */

/* 页表页一律通过 HHDM 访问（phys + hhdm）：
 * Limine 的 identity 映射不保证覆盖高位物理地址，用 phys 当 vaddr
 * 写入会落到错误物理页（阶段七实测 0x1fec4xxx 高位页）。 */
static inline void *phys_to_ptr(uint64_t phys)
{
    return (void *)(uintptr_t)(phys + hhdm_off);
}

/* ------------------------------------------------------------------ */
/* CR / MSR 访问                                                    */
/* ------------------------------------------------------------------ */
static inline uint64_t read_cr0(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}
static inline void write_cr0(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr0" :: "r"(v) : "memory");
}
static inline uint64_t read_cr4(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}
static inline void write_cr4(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr4" :: "r"(v) : "memory");
}

/* ------------------------------------------------------------------ */
/* 页表遍历辅助                                                    */
/* ------------------------------------------------------------------ */

/* 取父表 index 处的子表：不存在且 create 则分配并清零。返回子表（HHDM 访问）。 */
static struct page_entry *table_next(struct page_entry *parent, uint64_t index,
                                     bool create)
{
    struct page_entry *e = &parent[index];
    if (e->present)
        return phys_to_ptr(e->address << 12);

    if (!create)
        return NULL;
    uint64_t phys = buddy_alloc(0);
    if (phys == 0)
        return NULL;
    memset(phys_to_ptr(phys), 0, PAGE_SIZE);
    e->present = 1;
    e->rw = 1;
    e->address = phys >> 12;
    return phys_to_ptr(phys);
}

static struct page_entry *get_pml4(void)
{
    return phys_to_ptr(pml4_phys);
}

/* 4KB 映射 */
static void map_page_4k(uint64_t virt, uint64_t phys, uint64_t flags)
{
    struct page_entry *pml4 = get_pml4();
    struct page_entry *pdpt = table_next(pml4, (virt >> 39) & 0x1FF, true);
    struct page_entry *pd = table_next(pdpt, (virt >> 30) & 0x1FF, true);
    struct page_entry *pt = table_next(pd, (virt >> 21) & 0x1FF, true);
    assert(pt != NULL);
    struct page_entry *pte = &pt[(virt >> 12) & 0x1FF];

    pte->present = 1;
    pte->rw = (flags & VMM_WRITABLE) ? 1 : 0;
    pte->us = (flags & VMM_USER) ? 1 : 0;
    pte->address = phys >> 12;
    if (flags & VMM_NX)
        pte->nx = 1;

    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

/* 2MB 大页映射（phys 必须 2MB 对齐） */
static void map_page_2m(uint64_t virt, uint64_t phys, uint64_t flags)
{
    struct page_entry *pml4 = get_pml4();
    struct page_entry *pdpt = table_next(pml4, (virt >> 39) & 0x1FF, true);
    struct page_entry *pd = table_next(pdpt, (virt >> 30) & 0x1FF, true);
    assert(pd != NULL);
    struct page_entry *pde = &pd[(virt >> 21) & 0x1FF];

    pde->present = 1;
    pde->rw = (flags & VMM_WRITABLE) ? 1 : 0;
    pde->us = (flags & VMM_USER) ? 1 : 0;
    pde->ps_pat = 1;               /* 2MB 大页 */
    pde->address = phys >> 12;
}

/* ------------------------------------------------------------------ */
/* 公开接口                                                        */
/* ------------------------------------------------------------------ */

int vmm_init(uint64_t kernel_phys_base, uint64_t kernel_virt_base,
             uint64_t hhdm_offset)
{
    hhdm_off = hhdm_offset;
    kernel_phys_base_g = kernel_phys_base;

    /* 1. PML4 */
    pml4_phys = buddy_alloc(0);
    assert(pml4_phys != 0);
    memset(phys_to_ptr(pml4_phys), 0, PAGE_SIZE);

    /* 2. identity [0, 4GB) 2MB 大页（启动过渡 + 页表/伙伴链表访问） */
    for (uint64_t i = 0; i < (4ull << 30); i += (2ull << 20))
        map_page_2m(i, i, VMM_WRITABLE);

    /* 3. HHDM [hhdm, hhdm+4GB) -> phys [0, 4GB) 2MB 大页
     *    （覆盖 PMM 位图 @hhdm+0x1000、GOP 帧缓冲——UEFI 下常在
     *     phys 0x80000000 附近，BIOS 模式下 Limine 可能把帧缓冲放在
     *     phys 0xfd000000（约 4GB 处），故映射满 4GB） */
    for (uint64_t i = 0; i < (4ull << 30); i += (2ull << 20))
        map_page_2m(hhdm_off + i, i, VMM_WRITABLE);

    /* 4. 内核高半区：4KB 页（内核物理基址非 2MB 对齐，无法用大页）。
     *    映射整个内核镜像（含 .rodata 内嵌的 27MB 文渊黑体字体），
     *    大小由链接器符号 _kernel_end 给出。 */
    extern uint8_t _kernel_end[];
    uint64_t kernel_size = (uint64_t)(uintptr_t)_kernel_end - kernel_virt_base;
    for (uint64_t off = 0; off < kernel_size; off += PAGE_SIZE)
        map_page_4k(kernel_virt_base + off, kernel_phys_base + off,
                    VMM_WRITABLE);
    /* 诊断：打印 kernel_size 与关键页映射（安装器 memcpy 源区曾 #PF） */
    {
        extern void uart_printf(const char *fmt, ...);
        uart_printf("[Nova] vmm: kernel_size=0x%llx phys_base=0x%llx\r\n",
                    (unsigned long long)kernel_size,
                    (unsigned long long)kernel_phys_base);
        uint64_t probes[] = { 0xffffffff81e00000ull, 0xffffffff82000000ull,
                              0xffffffff82e00000ull, 0xffffffff83000000ull };
        for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
            uint64_t p = virt_to_phys(probes[i]);
            uart_printf("[Nova] vmm: map %p -> %p (%s)\r\n",
                        (void *)probes[i], (void *)p,
                        p ? "ok" : "MISSING");
        }
    }

    /* 5. 用户区（阶段十二）：ring3 代码与栈。
     *    均为 2MB 对齐的空闲物理区（低于内核物理基址），
     *    在恒等映射上叠加 U|W 标志（代码页无 NX，可执行）。
     *    注意：用户访问要求每一级页表项都有 U 位（PML4/PDP 亦需）。 */
    map_page_2m(NOVA_USER_CODE_BASE, NOVA_USER_CODE_BASE,
                VMM_WRITABLE | VMM_USER);     /* [0x400000, 0x600000) */
    map_page_2m(NOVA_USER_STACK_BASE, NOVA_USER_STACK_BASE,
                VMM_WRITABLE | VMM_USER);     /* [0x7E00000, 0x8000000) */

    {
        /* 用户区位于 PML4[0] -> PDP[0]：给这两级置 U 位。
         * 叶子仍只有显式标 U 的页（用户代码/栈）可被 ring3 访问，
         * 其余恒等映射叶子保持 supervisor，不受影响。 */
        struct page_entry *pml4 = get_pml4();
        struct page_entry *pdpt = table_next(pml4, 0, false);
        if (pdpt != NULL) {
            pml4[0].us = 1;
            pdpt[0].us = 1;
        }
    }

    /* 6. 分页相关控制寄存器（长模式/分页 Limine 已开启，按规范显式设置） */
    uint64_t cr4 = read_cr4();
    cr4 |= (1u << 5);                       /* PAE */
    write_cr4(cr4);

    uint64_t efer = rdmsr(0xC0000080);
    efer |= (1u << 8) | (1u << 11);         /* LME | NXE */
    wrmsr(0xC0000080, efer);

    write_cr3(pml4_phys);                   /* 切换页表（隐式刷新 TLB） */

    uint64_t cr0 = read_cr0();
    cr0 |= (1u << 31);                      /* PG */
    write_cr0(cr0);

    return 0;
}

uint64_t vmm_alloc_page(uint64_t virt_addr)
{
    uint64_t phys = buddy_alloc(0);
    if (phys == 0)
        return 0;
    map_page_4k(virt_addr, phys, VMM_WRITABLE);
    return phys;
}

uint64_t vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags)
{
    map_page_4k(virt_addr, phys_addr, flags);
    return phys_addr;
}

uint64_t virt_to_phys(uint64_t virt)
{
    struct page_entry *pml4 = get_pml4();
    struct page_entry *e = &pml4[(virt >> 39) & 0x1FF];
    if (!e->present)
        return 0;
    struct page_entry *pdpt = phys_to_ptr(e->address << 12);
    e = &pdpt[(virt >> 30) & 0x1FF];
    if (!e->present)
        return 0;
    struct page_entry *pd = phys_to_ptr(e->address << 12);
    e = &pd[(virt >> 21) & 0x1FF];
    if (!e->present)
        return 0;
    if (e->ps_pat)
        return (e->address << 12) + (virt & 0x1FFFFF);   /* 2MB 大页 */
    struct page_entry *pt = phys_to_ptr(e->address << 12);
    e = &pt[(virt >> 12) & 0x1FF];
    if (!e->present)
        return 0;
    return (e->address << 12) + (virt & 0xFFF);
}

/* 页表遍历诊断（panic 用）：打印各级页表项，返回解析出的物理地址（0=缺失） */
uint64_t vmm_walk(uint64_t virt)
{
    extern void uart_printf(const char *fmt, ...);
    struct page_entry *pml4 = get_pml4();
    struct page_entry *e = &pml4[(virt >> 39) & 0x1FF];
    uart_printf("[Nova] walk: PML4[%u]=%llx p=%u\n",
                (unsigned)((virt >> 39) & 0x1FF),
                (unsigned long long)*(uint64_t *)e, (unsigned)e->present);
    if (!e->present)
        return 0;
    struct page_entry *pdpt = phys_to_ptr(e->address << 12);
    e = &pdpt[(virt >> 30) & 0x1FF];
    uart_printf("[Nova] walk: PDP[%u]=%llx p=%u\n",
                (unsigned)((virt >> 30) & 0x1FF),
                (unsigned long long)*(uint64_t *)e, (unsigned)e->present);
    if (!e->present)
        return 0;
    struct page_entry *pd = phys_to_ptr(e->address << 12);
    e = &pd[(virt >> 21) & 0x1FF];
    uart_printf("[Nova] walk: PD[%u]=%llx p=%u ps=%u\n",
                (unsigned)((virt >> 21) & 0x1FF),
                (unsigned long long)*(uint64_t *)e, (unsigned)e->present,
                (unsigned)e->ps_pat);
    if (!e->present)
        return 0;
    if (e->ps_pat)
        return (e->address << 12) + (virt & 0x1FFFFF);
    struct page_entry *pt = phys_to_ptr(e->address << 12);
    e = &pt[(virt >> 12) & 0x1FF];
    uart_printf("[Nova] walk: PT[%u]=%llx p=%u\n",
                (unsigned)((virt >> 12) & 0x1FF),
                (unsigned long long)*(uint64_t *)e, (unsigned)e->present);
    if (!e->present)
        return 0;
    return (e->address << 12) + (virt & 0xFFF);
}

uint64_t phys_to_virt(uint64_t phys)
{
    return hhdm_off + phys;
}

uint64_t vmm_get_cr3(void)
{
    return read_cr3();
}

/* 重建内核镜像某一虚拟页的映射（防御：长运行下页表项偶发损坏/缺失，
 * 安装器 memcpy 源在 .rodata.sysimg 区域曾 #PF）。返回物理地址，0=非法 */
uint64_t vmm_fix_kernel_page(uint64_t virt)
{
    extern uint8_t _kernel_end[];
    if (virt < 0xffffffff80000000ull ||
        virt >= (uint64_t)(uintptr_t)_kernel_end)
        return 0;
    uint64_t off = virt - 0xffffffff80000000ull;
    uint64_t phys = kernel_phys_base_g + off;
    map_page_4k(virt, phys, VMM_WRITABLE);
    return phys;
}
