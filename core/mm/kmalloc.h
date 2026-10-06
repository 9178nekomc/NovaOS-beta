/* kernel/mm/kmalloc.h - Nova OS 阶段八：内核堆分配器接口
 *
 * 分层：
 *   size <= 4096 : 8 级 slab 对象池（32/64/128/256/512/1024/2048/4096 字节），
 *                  每池由 buddy 页切分，空闲对象链表管理
 *   size >  4096 : buddy_alloc(size_to_order(size+8))，页头 8 字节记录 order
 *                  （size+8：页首 8 字节被 order 头占用，必须多分 8 字节，
 *                   否则页对齐尺寸会越界写坏相邻内存）
 *
 * 返回虚拟地址（HHDM 映射：vmm 页表已建立直接映射）。
 */
#ifndef NOVA_KMALLOC_H
#define NOVA_KMALLOC_H

#include <stddef.h>
#include <stdint.h>

/*
 * 初始化堆分配器。
 * @hhdm_offset 高半区直接映射偏移（对象虚拟地址 = 物理 + hhdm）
 * 返回 0 成功。
 */
int kmalloc_init(uint64_t hhdm_offset);

/* 分配 size 字节；失败返回 NULL */
void *kmalloc(size_t size);

/* 释放（自动识别 slab 对象 / 大对象） */
void kfree(void *ptr);

/* 调整大小：原地扩展/收缩；需要搬移时分配新块并拷贝 */
void *krealloc(void *ptr, size_t size);

/* n*size 字节清零分配 */
void *kcalloc(size_t n, size_t size);

/* 查询对象实际分配大小 */
size_t kmalloc_size(void *ptr);

/* 当前活跃（未释放）对象数（泄漏自检用） */
uint64_t kmalloc_active_objects(void);

/* 缓存命中率（0-100） */
uint64_t kmalloc_hit_rate(void);

/* 打印统计（串口）；终端统计由调用方按需输出 */
void kmalloc_print_stats(void);

#endif /* NOVA_KMALLOC_H */
