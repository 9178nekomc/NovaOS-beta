/* kernel/mm/pmm.c - Nova OS 阶段五：位图物理内存分配器实现
 *
 * 流程：
 *   1. 遍历 Limine 内存映射，仅收集 LIMINE_MEMMAP_USABLE 区域
 *   2. 以最高可用区终点计算总页数，位图大小 = total_pages/8 字节
 *   3. 位图放置在最低可用区（>=1MB）头部，其自身页保持已用
 *   4. 位图初始全部置 1（已用），再按可用区逐页清 0（空闲），
 *      跳过：页 0、1MB 以下固件区、位图自身页
 *
 * 分配：首次适应扫描位图；释放：清 bit 并 assert 前置状态。
 * 单核阶段无锁；多核阶段（十一）补充自旋锁。
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <limine.h>

#include "pmm.h"
#include "../types/assert.h"
#include "../../lib/uart.h"
#include "../../graphics/terminal/terminal.h"

#define PMM_MAX_REGIONS 64
#define LOW_MEM_END_PAGE 256      /* 1MB 以下固件区（IVT/BDA/EBDA）永不分配 */

struct pmm_region {
    uint64_t base;
    uint64_t length;
};

static struct pmm_region regions[PMM_MAX_REGIONS];
static int region_count;

static uint8_t *bitmap;           /* 虚拟地址（phys + hhdm_offset） */
static uint64_t bitmap_phys;
static uint64_t total_pages;
static uint64_t free_pages;
static uint64_t hhdm;
static uint64_t scan_hint;        /* 首次适应扫描游标（避免每轮从 0 全表扫） */

static inline bool page_is_used(uint64_t page)
{
    return (bitmap[page / 8] & (uint8_t)(1u << (page % 8))) != 0;
}

static inline void page_set_used(uint64_t page)
{
    bitmap[page / 8] |= (uint8_t)(1u << (page % 8));
}

static inline void page_set_free(uint64_t page)
{
    bitmap[page / 8] &= (uint8_t)~(1u << (page % 8));
}

int pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset,
             uint64_t kernel_phys_base, uint64_t kernel_virt_base)
{
    if (memmap == NULL)
        return -EINVAL;

    hhdm = hhdm_offset;

    /* 1. 收集可用区域，并找最低可用基址与最高末端 */
    region_count = 0;
    uint64_t lowest_base = UINT64_MAX;
    uint64_t max_end = 0;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;

        assert(region_count < PMM_MAX_REGIONS);
        regions[region_count].base = e->base;
        regions[region_count].length = e->length;
        region_count++;

        if (e->base < lowest_base)
            lowest_base = e->base;
        uint64_t end = e->base + e->length;
        if (end > max_end)
            max_end = end;
    }
    assert(region_count > 0);
    assert(lowest_base >= PAGE_SIZE);   /* 位图必须落在 >=1MB 处 */

    /* 2. 总页数与位图尺寸 */
    total_pages = (max_end + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t bitmap_size = (total_pages + 7) / 8;
    uint64_t bitmap_pages = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;

    /* 3. 位图放在最低可用区头部 */
    bitmap_phys = lowest_base;
    bitmap = (uint8_t *)(uintptr_t)(bitmap_phys + hhdm);
    assert(bitmap_pages * PAGE_SIZE <= regions[0].length ||
           bitmap_phys + bitmap_pages * PAGE_SIZE <=
               lowest_base + regions[0].length);   /* 最低区放得下位图 */

    /* 4. 初始全部已用 */
    memset(bitmap, 0xFF, bitmap_size);

    /* 5. 按可用区逐页标记空闲（跳过保护页） */
    free_pages = 0;
    for (int r = 0; r < region_count; r++) {
        uint64_t start_page = regions[r].base / PAGE_SIZE;
        uint64_t end_page = (regions[r].base + regions[r].length + PAGE_SIZE - 1)
                            / PAGE_SIZE;
        for (uint64_t p = start_page; p < end_page && p < total_pages; p++) {
            if (p < LOW_MEM_END_PAGE)          /* 1MB 以下固件区 */
                continue;
            if (p >= bitmap_phys / PAGE_SIZE &&
                p < bitmap_phys / PAGE_SIZE + bitmap_pages)  /* 位图自身 */
                continue;
            page_set_free(p);
            free_pages++;
        }
    }

    /* 6. 内核镜像占用的物理页必须标记已用！
     * Limine 的内存映射通常把内核镜像文件部分（.text/.rodata/.data）
     * 报告为保留，但 .bss（NOBITS，超出文件大小）所在的物理页可能被
     * 报告为 USABLE——若不加保护，buddy 会把内核 .bss 页当作空闲内存
     * 分配出去：kmalloc 返回的"堆对象"（hhdm 视图）与内核高半区视图
     * 指向同一物理页，FreeType 等写堆对象即写坏 .bss 中的静态表
     * （sfnt_interface / tt_cmap 类等），函数指针被覆盖成随机数据，
     * 表现为偶发 #GP（乱码 RIP）、字形渲染崩溃等。
     * 修复：把整个内核镜像虚拟范围对应的物理页全部标记已用。 */
    {
        extern char _kernel_end[];
        uint64_t kstart_phys = kernel_phys_base;
        uint64_t kend_phys = kernel_phys_base +
                             ((uint64_t)(uintptr_t)_kernel_end -
                              kernel_virt_base);
        uint64_t sp = kstart_phys / PAGE_SIZE;
        uint64_t ep = (kend_phys + PAGE_SIZE - 1) / PAGE_SIZE;
        for (uint64_t p = sp; p < ep && p < total_pages; p++)
            page_set_used(p);
        uart_printf("[Nova] PMM: kernel image reserved "
                    "phys=0x%llx-0x%llx (%llu pages)\r\n",
                    (unsigned long long)kstart_phys,
                    (unsigned long long)kend_phys,
                    (unsigned long long)(ep - sp));
    }

    return 0;
}

uint64_t pmm_alloc_page(void)
{
    for (uint64_t p = 0; p < total_pages; p++) {
        if (!page_is_used(p)) {
            page_set_used(p);
            assert(free_pages > 0);
            free_pages--;
            return p * PAGE_SIZE;
        }
    }
    return 0;   /* 内存耗尽 */
}

void pmm_free_page(uint64_t phys)
{
    assert(phys % PAGE_SIZE == 0);
    uint64_t p = phys / PAGE_SIZE;
    assert(p < total_pages);
    assert(page_is_used(p));          /* 必须确实处于已分配状态 */
    page_set_free(p);
    free_pages++;
}

uint64_t pmm_alloc_pages(uint64_t count)
{
    if (count == 0)
        return 0;

    /* 从游标开始扫描，回绕一圈未找到即失败 */
    uint64_t p = scan_hint;
    uint64_t run = 0;
    uint64_t scanned = 0;

    for (;;) {
        if (p < total_pages && !page_is_used(p)) {
            run++;
            if (run == count) {
                uint64_t start = p - count + 1;
                for (uint64_t q = start; q <= p; q++)
                    page_set_used(q);
                assert(free_pages >= count);
                free_pages -= count;
                scan_hint = p + 1;
                return start * PAGE_SIZE;
            }
        } else {
            run = 0;
        }
        p++;
        if (p >= total_pages)
            p = 0;                       /* 回绕 */
        if (++scanned > total_pages)
            break;                       /* 扫完一整圈 */
    }
    return 0;   /* 无连续块 */
}

void pmm_free_pages(uint64_t phys, uint64_t count)
{
    for (uint64_t i = 0; i < count; i++)
        pmm_free_page(phys + i * PAGE_SIZE);
}

uint64_t pmm_free_page_count(void)
{
    return free_pages;
}

uint64_t pmm_total_page_count(void)
{
    return total_pages;
}

void pmm_print_usage(void)
{
    uint64_t used_pages = total_pages - free_pages;
    uint64_t total_mb = total_pages * PAGE_SIZE / (1024u * 1024u);
    uint64_t used_mb = used_pages * PAGE_SIZE / (1024u * 1024u);
    uint64_t pct = used_pages * 100u / (total_pages ? total_pages : 1u);

    terminal_set_color(TERM_COLOR_BRIGHT_CYAN, TERM_COLOR_BLACK);
    terminal_printf("[INFO] ");
    terminal_set_color(TERM_COLOR_LIGHT_GRAY, TERM_COLOR_BLACK);
    terminal_printf("PMM: total=%uMB used=%uMB (%u%%), free=%u pages, bitmap@%p\n",
                    (unsigned)total_mb, (unsigned)used_mb, (unsigned)pct,
                    (unsigned)free_pages, (void *)bitmap_phys);

    uart_printf("[Nova] PMM: total=%uMB used=%uMB (%u%%) free=%u pages bitmap@%p\r\n",
                (unsigned)total_mb, (unsigned)used_mb, (unsigned)pct,
                (unsigned)free_pages, (void *)bitmap_phys);
}
