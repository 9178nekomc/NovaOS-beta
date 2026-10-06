/* kernel/mm/pmm.h - Nova OS 阶段五：物理内存管理器接口
 *
 * 基于 Limine 内存映射构建 4KB 页位图（每 bit 一页）：
 *   - 仅将 LIMINE_MEMMAP_USABLE 区域标记为空闲
 *   - 位图本身、页 0 与 1MB 以下固件区、内核/模块/帧缓冲区域保持已用
 *
 * 返回值为物理地址（未启用内核页表映射前可直接以身份映射/HHDM 访问）。
 */
#ifndef NOVA_PMM_H
#define NOVA_PMM_H

#include <stdint.h>
#include <limine.h>

#define PAGE_SIZE 4096u

/*
 * 初始化位图分配器。
 * @memmap      Limine 内存映射响应（memmap_request.response）
 * @hhdm_offset 高半区直接映射偏移（hhdm_request.response->offset），
 *              用于将位图所在物理页映射到可写虚拟地址
 * @kernel_phys_base / @kernel_virt_base 内核镜像物理/虚拟基址
 *              （kernel_address_request.response），用于把内核镜像
 *              物理页标记为已用（Limine 可能把 .bss 所在页报告为
 *              USABLE，不标记会被 buddy 当空闲内存分配，写坏 .bss）
 * 返回 0 成功；失败返回负 errno。
 */
int pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset,
             uint64_t kernel_phys_base, uint64_t kernel_virt_base);

/* 分配一页（4KB），返回物理地址；无可用页返回 0 */
uint64_t pmm_alloc_page(void);

/* 释放一页；assert：地址按页对齐、在范围内、且确实处于已分配状态 */
void pmm_free_page(uint64_t phys);

/* 分配 count 个连续页（首次适应），返回起始物理地址；失败返回 0 */
uint64_t pmm_alloc_pages(uint64_t count);

/* 连续释放 count 个页 */
void pmm_free_pages(uint64_t phys, uint64_t count);

/* 打印内存统计：总内存 / 已用页 / 空闲页 / 使用率（终端 + 串口） */
void pmm_print_usage(void);

/* 当前空闲页数（供泄漏自检） */
uint64_t pmm_free_page_count(void);

/* 阶段二十：总可用页数（进入分配池的页数，含已用） */
uint64_t pmm_total_page_count(void);

#endif /* NOVA_PMM_H */
