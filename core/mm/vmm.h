/* kernel/mm/vmm.h - Nova OS 阶段七：虚拟内存管理接口
 *
 * 四级页表（PML4/PDPT/PD/PT）：
 *   - 前 4GB identity 映射（2MB 大页，启动过渡）
 *   - HHDM 高半区直接映射（2MB 大页，覆盖位图/帧缓冲/所有物理内存）
 *   - 内核高半区映射（4KB 页：内核物理基址非 2MB 对齐）
 *   - vmm_alloc_page / vmm_map_page 按需建立 4KB 映射
 */
#ifndef NOVA_VMM_H
#define NOVA_VMM_H

#include <stdint.h>

/* 页表项标志 */
#define VMM_PRESENT   0x0001u
#define VMM_WRITABLE  0x0002u
#define VMM_USER      0x0004u
#define VMM_NX        0x0008u

/*
 * 初始化页表并启用分页：
 *   - 构建 identity [0,4GB) 与 HHDM [hhdm, hhdm+3GB) 的 2MB 大页映射
 *   - 内核 vaddr[kernel_virt_base, +8MB) -> phys[kernel_phys_base, +8MB) 4KB 映射
 *   - 设置 CR4.PAE、IA32_EFER.LME/NXE、CR3（新页表）、CR0.PG
 * 切换后当前执行流（内核高半区/栈/位图/帧缓冲/伙伴链表）全部继续有效。
 * 返回 0 成功；失败返回负 errno。
 */
int vmm_init(uint64_t kernel_phys_base, uint64_t kernel_virt_base,
             uint64_t hhdm_offset);

/* 分配一物理页（buddy_alloc(0)）并映射到 virt_addr（4KB PTE）。
 * 返回物理地址；失败返回 0。 */
uint64_t vmm_alloc_page(uint64_t virt_addr);

/* 手动映射：virt_addr -> phys_addr（4KB PTE，按需创建中间表）。
 * 返回 phys_addr；失败返回 0。 */
uint64_t vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);

/* 查询当前页表中虚拟地址对应的物理地址；未映射返回 0 */
uint64_t virt_to_phys(uint64_t virt_addr);

/* 页表遍历诊断（panic 用）：打印各级页表项，返回解析出的物理地址 */
uint64_t vmm_walk(uint64_t virt_addr);

/* 重建内核镜像虚拟页的映射（防御页表项损坏）；返回物理地址，0=非法 */
uint64_t vmm_fix_kernel_page(uint64_t virt_addr);

/* 物理地址 -> 高半区直接映射虚拟地址（hhdm + phys） */
uint64_t phys_to_virt(uint64_t phys_addr);

/* 当前 CR3（PML4 物理地址） */
uint64_t vmm_get_cr3(void);

#endif /* NOVA_VMM_H */
