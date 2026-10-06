/* kernel/ata/ata.h - Nova OS 阶段十四：ATA PIO 硬盘驱动接口
 *
 * 传统 IDE 通道（主 0x1F0 / 从 0x170），PIO 模式，LBA28 寻址。
 * 轮询式（无中断）：每次命令后等待 BSY/DRQ 状态（带超时防挂起）。
 */
#ifndef NOVA_ATA_H
#define NOVA_ATA_H

#include <stdint.h>

/* ATA 主通道主设备（当前仅使用 bus 0, drive 0） */
#define ATA_DRIVE_PRIMARY_MASTER 0

/*
 * 初始化：对指定设备发 IDENTIFY，填充型号与扇区数。
 * 返回 0 成功；-ENODEV 无设备；-EIO 失败。
 */
int ata_init(void);

/* 磁盘几何（ata_init 成功后有效；返回当前选中设备） */
uint64_t ata_sector_count(void);          /* 总扇区数（LBA28） */
const char *ata_model(void);              /* 型号字符串（40 字符） */

/* 阶段二十（多盘）：选择当前设备（0..ATA_MAX_DEV-1），返回 0 成功 */
int ata_select_device(int dev);

/* 可用设备数 */
int ata_device_count(void);

/* 探测槽位总数（0..ATA_MAX_DEV-1，含空槽；供上层按槽位遍历） */
int ata_max_devices(void);

/* 当前选中设备索引（未选中返回 -1） */
int ata_current_index(void);

/*
 * 读/写 count 个扇区（LBA28，count <= 256；buf 大小 count*512）。
 * 返回 0 成功；负 errno 失败。
 */
int ata_read_sectors(uint64_t lba, uint16_t count, void *buf);
int ata_write_sectors(uint64_t lba, uint16_t count, const void *buf);

#endif /* NOVA_ATA_H */
