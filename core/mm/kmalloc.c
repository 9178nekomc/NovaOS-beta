/* kernel/mm/kmalloc.c - Nova OS 阶段八：内核堆分配器实现
 *
 * slab 空闲链表：空闲对象的头 8 字节存放下一空闲对象地址（虚拟地址）。
 * 大对象：buddy_alloc 后，页首 8 字节记录 order，返回页首+8。
 *
 * kfree 路径识别：先扫描各 slab 缓存的对象页（指针所属物理页匹配），
 * 命中即回收到对应缓存；否则按大对象头中的 order 释放。
 *
 * 多核安全（阶段二十）：kmalloc/kfree 等公共接口以自旋锁 + 关中断
 * 串行化——FreeType 渲染等场景在多个上下文（任务/中断/SMP）频繁
 * 分配，此前无锁的 free_list 会被并发弹/压破坏（表现为偶发 #GP
 * 乱码 RIP、字形缓存损坏等），必须互斥。
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kmalloc.h"
#include "buddy.h"
#include "pmm.h"
#include "../types/assert.h"
#include "../../lib/io.h"
#include "../../lib/uart.h"
#include "../../lib/spinlock.h"

#define SLAB_COUNT     8
#define SLAB_MAX_PAGES 128

static const uint32_t slab_sizes[SLAB_COUNT] =
    { 32, 64, 128, 256, 512, 1024, 2048, 4096 };

struct slab_cache {
    uint32_t object_size;
    uint32_t objects_per_page;
    uint64_t free_list;         /* 空闲对象链表头（虚拟地址） */
    uint64_t pages[SLAB_MAX_PAGES];   /* 本缓存占用的物理页 */
    uint32_t page_count;
    uint64_t free_count;
    uint64_t alloc_count;
    uint64_t grow_count;        /* 扩容次数（miss） */
};

static struct slab_cache caches[SLAB_COUNT];
static uint64_t kmalloc_hhdm;

static spinlock_t kmalloc_lock = SPINLOCK_INIT;

static uint64_t stat_active_objects;
static uint64_t stat_alloc_bytes;
static uint64_t stat_hits;      /* 直接命中现有空闲对象 */
static uint64_t stat_misses;    /* 需要新开 slab 页 */

/* 关中断 + 加锁；返回调用方原 IF 状态（kmalloc_unlock 恢复） */
static unsigned long kmalloc_lock_irq(void)
{
    unsigned long flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags) : : "memory");
    cli();
    spin_lock(&kmalloc_lock);
    return flags;
}

static void kmalloc_unlock_irq(unsigned long flags)
{
    spin_unlock(&kmalloc_lock);
    if (flags & 0x200)
        sti();
}

static int cache_for_size(size_t size)
{
    for (int i = 0; i < SLAB_COUNT; i++)
        if (size <= slab_sizes[i])
            return i;
    return -1;
}

int kmalloc_init(uint64_t hhdm_offset)
{
    kmalloc_hhdm = hhdm_offset;

    for (int i = 0; i < SLAB_COUNT; i++) {
        caches[i].object_size = slab_sizes[i];
        caches[i].objects_per_page = PAGE_SIZE / slab_sizes[i];
        caches[i].free_list = 0;
        caches[i].page_count = 0;
        caches[i].free_count = 0;
        caches[i].alloc_count = 0;
        caches[i].grow_count = 0;
    }
    stat_active_objects = 0;
    stat_alloc_bytes = 0;
    stat_hits = 0;
    stat_misses = 0;
    return 0;
}

/* 新开一个 slab 页并切分对象入空闲链表 */
static int slab_grow(struct slab_cache *c)
{
    if (c->page_count >= SLAB_MAX_PAGES)
        return -ENOMEM;
    uint64_t phys = buddy_alloc(0);
    if (phys == 0)
        return -ENOMEM;

    uint64_t virt = phys + kmalloc_hhdm;
    c->pages[c->page_count++] = phys;

    for (uint32_t i = 0; i < c->objects_per_page; i++) {
        uint64_t obj = virt + i * c->object_size;
        *(uint64_t *)(uintptr_t)obj = c->free_list;
        c->free_list = obj;
        c->free_count++;
    }
    c->grow_count++;
    return 0;
}

/* 内部（调用方已持锁）版本 */
static void *kmalloc_locked(size_t size)
{
    if (size == 0)
        return NULL;

    if (size <= 4096) {
        int ci = cache_for_size(size);
        assert(ci >= 0);
        struct slab_cache *c = &caches[ci];

        if (c->free_list == 0) {
            if (slab_grow(c) != 0)
                return NULL;
            stat_misses++;
        } else {
            stat_hits++;
        }

        uint64_t obj = c->free_list;
        /* 完整性校验：堆被越界写破坏时尽早报告（而不是裸 #GP）。
         * slab 对象物理页可由 buddy 分配在任意位置（低内存），
         * 合法范围 = hhdm + [0, 4GB)。 */
        if (obj < kmalloc_hhdm ||
            obj >= kmalloc_hhdm + 0x100000000ull) {
            extern void uart_printf(const char *fmt, ...);
            uart_printf("[Nova] kmalloc: BAD free_list ci=%d size=%u "
                        "obj=%p (heap corrupted)\r\n",
                        ci, (unsigned)c->object_size, (void *)obj);
            for (;;)
                __asm__ volatile("hlt");
        }
        c->free_list = *(uint64_t *)(uintptr_t)obj;
        c->free_count--;
        c->alloc_count++;
        stat_active_objects++;
        stat_alloc_bytes += c->object_size;
        return (void *)(uintptr_t)obj;
    }

    /* 大对象：buddy + 页首 8 字节记录 order。
     * 注意：返回地址 = 页首 + 8，页首 8 字节被 order 头占用，
     * 可用空间 = order_to_size - 8。必须按 size+8 计算 order，
     * 否则页对齐尺寸（如 32768）会越界 8 字节写坏下一块
     * （历史根因：安装器 32KB 缓冲 memset 踩坏相邻页表页/堆块，
     *  表现为偶发 #PF、kmalloc BAD free_list、安装卡死）。 */
    int order = size_to_order(size + 8);
    uint64_t phys = buddy_alloc(order);
    if (phys == 0)
        return NULL;
    uint64_t virt = phys + kmalloc_hhdm;
    *(uint64_t *)(uintptr_t)virt = (uint64_t)order;
    stat_active_objects++;
    stat_alloc_bytes += size;
    return (void *)(uintptr_t)(virt + 8);
}

static void kfree_locked(void *ptr)
{
    if (ptr == NULL)
        return;

    uint64_t addr = (uint64_t)(uintptr_t)ptr;
    uint64_t page_phys = (addr & ~(uint64_t)(PAGE_SIZE - 1)) - kmalloc_hhdm;

    /* slab 路径：查找对象所属缓存与页 */
    for (int i = 0; i < SLAB_COUNT; i++) {
        struct slab_cache *c = &caches[i];
        for (uint32_t p = 0; p < c->page_count; p++) {
            if (c->pages[p] == page_phys) {
                *(uint64_t *)(uintptr_t)addr = c->free_list;
                c->free_list = addr;
                c->free_count++;
                stat_active_objects--;
                return;
            }
        }
    }

    /* 大对象路径 */
    uint64_t hdr = addr - 8;
    int order = (int)*(uint64_t *)(uintptr_t)hdr;
    if (order < 0 || order > BUDDY_MAX_ORDER ||
        ((hdr - kmalloc_hhdm) % order_to_size(order)) != 0)
        return;   /* 非法指针：静默丢弃（不应发生） */
    assert(order >= 0 && order <= BUDDY_MAX_ORDER);
    buddy_free(hdr - kmalloc_hhdm, order);
    stat_active_objects--;
}

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;

    unsigned long flags = kmalloc_lock_irq();
    void *p = kmalloc_locked(size);
    kmalloc_unlock_irq(flags);
    return p;
}

void kfree(void *ptr)
{
    if (ptr == NULL)
        return;

    unsigned long flags = kmalloc_lock_irq();
    kfree_locked(ptr);
    kmalloc_unlock_irq(flags);
}

/* 内部（调用方已持锁）版本；krealloc 使用 */
static size_t kmalloc_size_locked(void *ptr)
{
    if (ptr == NULL)
        return 0;
    uint64_t addr = (uint64_t)(uintptr_t)ptr;
    uint64_t page_phys = (addr & ~(uint64_t)(PAGE_SIZE - 1)) - kmalloc_hhdm;

    for (int i = 0; i < SLAB_COUNT; i++) {
        struct slab_cache *c = &caches[i];
        for (uint32_t p = 0; p < c->page_count; p++)
            if (c->pages[p] == page_phys)
                return c->object_size;
    }
    int order = (int)*(uint64_t *)(uintptr_t)(addr - 8);
    return (size_t)order_to_size(order);
}

size_t kmalloc_size(void *ptr)
{
    if (ptr == NULL)
        return 0;

    unsigned long flags = kmalloc_lock_irq();
    size_t sz = kmalloc_size_locked(ptr);
    kmalloc_unlock_irq(flags);
    return sz;
}

void *krealloc(void *ptr, size_t size)
{
    if (ptr == NULL)
        return kmalloc(size);

    unsigned long flags = kmalloc_lock_irq();

    size_t old = kmalloc_size_locked(ptr);
    void *np;
    if (size <= old) {
        np = ptr;                    /* 收缩：原地保留 */
    } else {
        np = kmalloc_locked(size);
        if (np == NULL) {
            kmalloc_unlock_irq(flags);
            return NULL;
        }
        memcpy(np, ptr, old);
        kfree_locked(ptr);
    }

    kmalloc_unlock_irq(flags);
    return np;
}

void *kcalloc(size_t n, size_t size)
{
    size_t total = n * size;
    void *p = kmalloc(total);
    if (p != NULL)
        memset(p, 0, total);
    return p;
}

uint64_t kmalloc_active_objects(void)
{
    return stat_active_objects;
}

uint64_t kmalloc_hit_rate(void)
{
    uint64_t denom = stat_hits + stat_misses;
    return denom ? stat_hits * 100 / denom : 0;
}

void kmalloc_print_stats(void)
{
    uart_printf("[Nova] kmalloc: active=%llu alloc=%lluB hits=%llu misses=%llu ",
                (unsigned long long)stat_active_objects,
                (unsigned long long)stat_alloc_bytes,
                (unsigned long long)stat_hits,
                (unsigned long long)stat_misses);
    for (int i = 0; i < SLAB_COUNT; i++)
        uart_printf("[%u]=%llu ", slab_sizes[i],
                    (unsigned long long)caches[i].free_count);
    uart_printf("\r\n");
}
