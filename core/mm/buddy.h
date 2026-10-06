/* kernel/mm/buddy.h - Nova OS 阶段六：Buddy System 分配器接口
 *
 * 管理 order 0-10 的 2 的幂次块（4KB .. 4MB），每 order 一条空闲链表。
 * 运行在 pmm 位图分配器之上，操作物理地址（不依赖分页）。
 *
 * 约定：
 *   - 块地址按 2^order 对齐（buddy 合并的前提）
 *   - buddy_alloc 返回物理地址（失败 0）
 *   - buddy_free 会与相邻同 order 空闲块合并并向上递归
 */
#ifndef NOVA_BUDDY_H
#define NOVA_BUDDY_H

#include <stdint.h>

#include "pmm.h"

#define BUDDY_MIN_ORDER 0
#define BUDDY_MAX_ORDER 10    /* 4KB << 10 = 4MB */

/*
 * 初始化：从 pmm 位图分配器取出所有内存，逐页入池并级联合并。
 * @hhdm_offset 高半区直接映射偏移：空闲链表节点位于物理块头部，
 *              必须经 HHDM 访问（Limine identity 映射不覆盖高位物理地址）
 * 返回 0 成功；失败返回负 errno。
 */
int buddy_init(uint64_t hhdm_offset);

/*
 * 分配 2^order 页的对齐块。
 * @order 0..10
 * 返回物理地址；无可用块返回 0。
 */
uint64_t buddy_alloc(int order);

/*
 * 释放 2^order 页的对齐块；与相邻空闲 buddy 合并并向上递归。
 * @phys 物理地址（必须按块大小对齐）
 * @order 0..10
 */
void buddy_free(uint64_t phys, int order);

/* order -> 字节大小（4KB << order） */
uint64_t order_to_size(int order);

/* 字节大小 -> 最小覆盖 order（向上取整） */
int size_to_order(uint64_t size);

/* 当前空闲块覆盖的总页数 */
uint64_t buddy_free_pages(void);

/* 打印统计：池大小 / 空闲 / 每 order 空闲块数（终端 + 串口） */
void buddy_print_stats(void);

#endif /* NOVA_BUDDY_H */
