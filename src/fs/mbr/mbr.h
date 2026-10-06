/* kernel/mbr/mbr.h - Nova OS 阶段十五：MBR 主引导记录解析接口
 *
 * MBR（扇区 0，512 字节）：
 *   [0, 446)     引导代码
 *   [446, 510)   4 个主分区表项（各 16 字节）
 *   [510, 512)   签名 0x55 0xAA
 */
#ifndef NOVA_MBR_H
#define NOVA_MBR_H

#include <stdint.h>

struct mbr_partition {
    uint8_t boot_flag;      /* 0x80 = 活动分区 */
    uint8_t chs_start[3];   /* 起始 CHS（本阶段不用） */
    uint8_t type;           /* 分区类型（0x83 Linux 等） */
    uint8_t chs_end[3];
    uint32_t lba_start;     /* 起始 LBA（小端） */
    uint32_t sectors;       /* 扇区数（小端） */
} __attribute__((packed));

struct mbr {
    uint8_t boot_code[446];
    struct mbr_partition parts[4];
    uint16_t signature;     /* 0xAA55（小端存储 0x55 0xAA） */
} __attribute__((packed));

#define MBR_SIGNATURE 0xAA55u

/*
 * 从 ATA 磁盘读取扇区 0 到 m（512 字节）。
 * 返回 0 成功；负 errno 失败。
 */
int mbr_read(struct mbr *m);

/*
 * 校验 MBR 签名（0xAA55）。
 * 返回 1 有效；0 无效。
 */
int mbr_validate(const struct mbr *m);

/* 统计主分区数（type != 0 的表项） */
int mbr_count_parts(const struct mbr *m);

/*
 * 构造示例 MBR（3 个主分区：0x83/0x0C/0x07，带 0xAA55 签名）。
 * 测试盘无分区表时由内核写入后再解析。
 */
void mbr_build_sample(struct mbr *m);

#endif /* NOVA_MBR_H */
