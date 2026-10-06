/* kernel/mm/buddy.c - Nova OS 阶段六：Buddy System 分配器实现
 *
 * 空闲链表节点直接存放在空闲块头部（空闲块内存本身就是载荷区）：
 *   struct buddy_block { next } 位于块起始地址。
 *
 * 初始化：从 pmm 以 2^MAX_ORDER 页为单位取出连续内存，仅接受按块大小
 * 对齐的地址（否则退回给 pmm，改用更小 order 继续），拆块入链。
 *
 * 分裂：buddy_alloc 从更高 order 取块，右半逐级加入低 order 链表。
 * 合并：buddy_free 计算 buddy = phys ^ size，若在链表中则摘除并递归。
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "buddy.h"
#include "pmm.h"
#include "../types/assert.h"
#include "../../lib/uart.h"
#include "../../graphics/terminal/terminal.h"

struct buddy_block {
    struct buddy_block *next;
};

static struct buddy_block *free_lists[BUDDY_MAX_ORDER + 1];
static uint64_t free_count[BUDDY_MAX_ORDER + 1];
static uint64_t pool_pages;    /* 进入 buddy 的总页数 */
static uint64_t bhhdm;         /* 空闲链表节点经 HHDM 访问 */

static inline void *bphys_to_ptr(uint64_t phys)
{
    return (void *)(uintptr_t)(phys + bhhdm);
}

static void buddy_add_block(uint64_t phys, int order)
{
    struct buddy_block *b = bphys_to_ptr(phys);
    b->next = free_lists[order];
    free_lists[order] = b;
    free_count[order]++;
}

/* 从链表摘除 target（按地址匹配）；找到并摘除返回 true */
static bool list_remove(struct buddy_block **head, struct buddy_block *target)
{
    struct buddy_block **pp = head;
    while (*pp != NULL) {
        if (*pp == target) {
            *pp = target->next;
            return true;
        }
        pp = &(*pp)->next;
    }
    return false;
}

uint64_t order_to_size(int order)
{
    if (order < BUDDY_MIN_ORDER)
        order = BUDDY_MIN_ORDER;
    if (order > BUDDY_MAX_ORDER)
        order = BUDDY_MAX_ORDER;
    return (1ull << order) * PAGE_SIZE;
}

int size_to_order(uint64_t size)
{
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    int order = 0;
    while ((1ull << order) < pages)
        order++;
    if (order > BUDDY_MAX_ORDER)
        order = BUDDY_MAX_ORDER;
    return order;
}

int buddy_init(uint64_t hhdm_offset)
{
    bhhdm = hhdm_offset;
    for (int o = 0; o <= BUDDY_MAX_ORDER; o++) {
        free_lists[o] = NULL;
        free_count[o] = 0;
    }
    pool_pages = 0;

    /* 从 pmm 逐页取走全部内存，用 buddy_free 的合并逻辑构建最优池：
     * 相邻页会级联合并成高阶大块（order 最大可达 10/4MB）。
     * 注意用带扫描游标的 pmm_alloc_pages(1)，避免每页全表扫描。 */
    for (;;) {
        uint64_t phys = pmm_alloc_pages(1);
        if (phys == 0)
            break;                       /* pmm 耗尽 */
        buddy_free(phys, 0);             /* 入池并向上合并 */
        pool_pages++;
    }

    if (pool_pages == 0)
        return -ENOMEM;
    return 0;
}

uint64_t buddy_alloc(int order)
{
    if (order < BUDDY_MIN_ORDER || order > BUDDY_MAX_ORDER)
        return 0;

    /* 从目标 order 起向上找首个非空链表 */
    for (int o = order; o <= BUDDY_MAX_ORDER; o++) {
        struct buddy_block *b = free_lists[o];
        if (b == NULL)
            continue;

        free_lists[o] = b->next;
        assert(free_count[o] > 0);
        free_count[o]--;

        uint64_t phys = (uint64_t)(uintptr_t)b - bhhdm;   /* 节点 -> 物理 */

        /* 逐级分裂：右半加入低一阶链表，左半继续 */
        while (o > order) {
            o--;
            buddy_add_block(phys + order_to_size(o), o);
        }
        return phys;
    }
    return 0;   /* 无可用块 */
}

void buddy_free(uint64_t phys, int order)
{
    if (order < BUDDY_MIN_ORDER || order > BUDDY_MAX_ORDER) {
        assert(!"buddy_free: bad order");
        return;
    }
    if (phys % order_to_size(order) != 0)
        return;
    assert(phys != 0);

    /* 与相邻 buddy 合并，向上递归 */
    while (order < BUDDY_MAX_ORDER) {
        uint64_t size = order_to_size(order);
        uint64_t buddy_phys = phys ^ size;   /* buddy 地址 */
        struct buddy_block *buddy = bphys_to_ptr(buddy_phys);

        if (!list_remove(&free_lists[order], buddy))
            break;                           /* buddy 不空闲：停止合并 */

        assert(free_count[order] > 0);
        free_count[order]--;
        if (buddy_phys < phys)
            phys = buddy_phys;               /* 合并后块取低地址 */
        order++;
    }

    buddy_add_block(phys, order);
}

uint64_t buddy_free_pages(void)
{
    uint64_t pages = 0;
    for (int o = 0; o <= BUDDY_MAX_ORDER; o++)
        pages += free_count[o] << o;
    return pages;
}

void buddy_print_stats(void)
{
    uint64_t free_pages = buddy_free_pages();
    uint64_t pool_mb = pool_pages * PAGE_SIZE / (1024u * 1024u);
    uint64_t free_mb = free_pages * PAGE_SIZE / (1024u * 1024u);
    uint64_t pct = free_pages * 100u / (pool_pages ? pool_pages : 1u);

    /* 终端：单行紧凑统计（避免超过 80 列换行打乱布局） */
    terminal_set_color(TERM_COLOR_BRIGHT_CYAN, TERM_COLOR_BLACK);
    terminal_printf("[INFO] ");
    terminal_set_color(TERM_COLOR_LIGHT_GRAY, TERM_COLOR_BLACK);
    terminal_printf("Buddy: pool=%uMB free=%uMB (%u%%)\n",
                    (unsigned)pool_mb, (unsigned)free_mb, (unsigned)pct);

    /* 串口：完整每 order 统计 */
    uart_printf("[Nova] Buddy: orders 0-%d pool=%uMB free=%uMB (%u%%) ",
                BUDDY_MAX_ORDER, (unsigned)pool_mb, (unsigned)free_mb,
                (unsigned)pct);
    for (int o = 0; o <= BUDDY_MAX_ORDER; o++)
        uart_printf("[%d]=%llu ", o, (unsigned long long)free_count[o]);
    uart_printf("\r\n");
}
