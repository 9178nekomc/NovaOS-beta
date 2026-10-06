/* kernel/ata/ata.c - Nova OS 阶段十四/二十：ATA PIO 驱动实现
 *
 * 主通道（0x1F0-0x1F7）+ 从通道（0x170-0x177），每通道主/从设备。
 * PIO 模式：
 *   - IDENTIFY (0xEC)：返回 256 字设备参数（词 60-61 = LBA28 扇区数，
 *     词 27-46 = 型号 ASCII）
 *   - READ SECTORS (0x20) / WRITE SECTORS (0x30)：LBA28，每扇区
 *     通过 16 位 data 端口传 256 字
 * 所有轮询带迭代上限（BSY/DRQ 超时返回 -EIO，避免设备失效挂死内核）。
 * 阶段二十：支持最多 4 个设备（dev 0..3 = 主通道主/从、从通道主/从），
 * 供多盘（C: 系统 + D:/E: 物理盘）使用。
 */
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "ata.h"
#include "../../lib/io.h"
#include "../../lib/uart.h"

#define ATA_BASE_PRIMARY   0x1F0u
#define ATA_BASE_SECONDARY 0x170u

#define ATA_MAX_DEV 4

#define CMD_IDENTIFY   0xECu
#define CMD_READ_SECT  0x20u
#define CMD_WRITE_SECT 0x30u

#define STS_BSY 0x80u
#define STS_DRQ 0x08u
#define STS_ERR 0x01u

#define DRIVE_SEL_LBA 0xE0u          /* LBA 模式 + 主设备 */

/* 轮询上限：TCG 慢速环境（GTK 重绘/杀软扫描等）下写入完成可能延迟数百 ms，
 * 1M 太紧会导致偶发 -EIO 中断安装；4M ≈ 1-2s 客机时间，配合写入重试。 */
#define POLL_MAX 4000000u

struct ata_dev {
    uint16_t base;       /* 通道基址（0x1F0 / 0x170） */
    uint8_t  drive_bit;  /* REG_DRIVE 的 bit4：0=主 0x10=从 */
    uint64_t sector_count;
    char     model[41];
    int      valid;
};

static struct ata_dev devs[ATA_MAX_DEV];
static int dev_count;

static struct ata_dev *cur_dev;      /* 当前读写目标设备 */

static uint16_t REG_DATA(uint16_t base)          { return base + 0; }
static uint16_t REG_SECTCOUNT(uint16_t base)     { return base + 2; }
static uint16_t REG_LBA_LOW(uint16_t base)       { return base + 3; }
static uint16_t REG_LBA_MID(uint16_t base)       { return base + 4; }
static uint16_t REG_LBA_HIGH(uint16_t base)      { return base + 5; }
static uint16_t REG_DRIVE(uint16_t base)         { return base + 6; }
static uint16_t REG_CMD(uint16_t base)           { return base + 7; }
static uint16_t REG_CTRL(uint16_t base)          { return base + 0x206; }

/* 轮询让步：io_wait 短延时（纯轮询）。
 * 注意：不要用 hlt 让步——实测（QEMU TCG）hlt 后依赖时钟中断唤醒，
 * 在安装长写入路径上会导致 main loop 饥饿/挂起；纯轮询 + cache=unsafe
 * 下 QEMU 同步完成写入，轮询立即可见。 */
static void ata_yield(void)
{
    io_wait();
}

/* 等待 BSY 清零（设备空闲）；超时返回 -EIO */
static int ata_wait_not_busy(void)
{
    for (uint32_t i = 0; i < POLL_MAX; i++) {
        uint8_t st = inb(REG_CMD(cur_dev->base));
        if (!(st & STS_BSY))
            return 0;
        if ((i & 0x3FFFu) == 0)
            ata_yield();
    }
    return -EIO;
}

/* 等待 DRQ 置位（数据就绪）；超时返回 -EIO */
static int ata_wait_drq(void)
{
    for (uint32_t i = 0; i < POLL_MAX; i++) {
        uint8_t st = inb(REG_CMD(cur_dev->base));
        if (st & STS_ERR)
            return -EIO;
        if (!(st & STS_BSY) && (st & STS_DRQ))
            return 0;
        if ((i & 0x3FFFu) == 0)
            ata_yield();
    }
    return -EIO;
}

/* 软复位 + 选择设备（首次 IDENTIFY 前）。
 *
 * 关键点：QEMU 的 SRST 是异步完成的（主循环 bottom-half 在收到
 * CTRL=0x04 后置 BUSY_STAT，稍后才真正复位）；复位完成前写入的
 * 命令块寄存器会被 QEMU 丢弃。因此必须轮询状态直到 BSY 清零，
 * 再写 DRIVE_SEL / IDENTIFY，否则会随机出现 "IDENTIFY no DRQ"。
 * 对主/从设备一视同仁：SRST 复位整条通道（主+从），随后选择目标。 */
static void ata_select_drive(void)
{
    outb(REG_CTRL(cur_dev->base), 0x04);   /* SRST */
    io_wait();
    outb(REG_CTRL(cur_dev->base), 0x00);
    io_wait();
    /* 等待复位完成（BSY 清零；空槽位状态恒为 0，立即通过） */
    for (uint32_t i = 0; i < POLL_MAX; i++) {
        if (!(inb(REG_CMD(cur_dev->base)) & STS_BSY))
            break;
        if ((i & 0x3FFu) == 0)
            ata_yield();
    }
    /* 选择目标设备（主/从） */
    outb(REG_DRIVE(cur_dev->base),
         (uint8_t)(DRIVE_SEL_LBA | cur_dev->drive_bit));
    io_wait();
    for (uint32_t i = 0; i < POLL_MAX; i++) {
        if (!(inb(REG_CMD(cur_dev->base)) & STS_BSY))
            break;
        if ((i & 0x3FFu) == 0)
            ata_yield();
    }}

uint64_t ata_sector_count(void)
{
    return cur_dev ? cur_dev->sector_count : 0;
}

const char *ata_model(void)
{
    return cur_dev ? cur_dev->model : "";
}

/* 选择当前设备（dev 0..ATA_MAX_DEV-1）；返回 0 成功 */
int ata_select_device(int dev)
{
    if (dev < 0 || dev >= ATA_MAX_DEV || !devs[dev].valid)
        return -ENODEV;
    cur_dev = &devs[dev];
    return 0;
}

/* 枚举可用设备数 */
int ata_device_count(void)
{
    return dev_count;
}

/* 探测槽位总数（0..ATA_MAX_DEV-1，含空槽；供上层按槽位遍历） */
int ata_max_devices(void)
{
    return ATA_MAX_DEV;
}

/* 当前选中设备索引（未选中返回 -1） */
int ata_current_index(void)
{
    if (cur_dev == NULL)
        return -1;
    return (int)(cur_dev - devs);
}

int ata_init(void)
{
    static const uint16_t bases[2] = { ATA_BASE_PRIMARY, ATA_BASE_SECONDARY };
    static const uint8_t  drivebits[2] = { 0x00, 0x10 };

    dev_count = 0;
    for (int b = 0; b < 2; b++) {
        for (int d = 0; d < 2; d++) {
            int devno = b * 2 + d;
            struct ata_dev *dev = &devs[devno];
            dev->base = bases[b];
            dev->drive_bit = drivebits[d];
            dev->valid = 0;
            cur_dev = dev;

            uint16_t id[256];
            int done = 0;
            ata_select_drive();

            /* 从设备最多重试一次（重新选择 + 再发 IDENTIFY） */
            for (int attempt = 0; attempt < 2 && !done; attempt++) {
                outb(REG_SECTCOUNT(dev->base), 0);
                outb(REG_LBA_LOW(dev->base), 0);
                outb(REG_LBA_MID(dev->base), 0);
                outb(REG_LBA_HIGH(dev->base), 0);
                outb(REG_CMD(dev->base), CMD_IDENTIFY);
                if (ata_wait_not_busy() != 0) {
                    uart_printf("[Nova] ATA: dev%d IDENTIFY busy timeout\r\n",
                                devno);
                    break;
                }
                uint8_t st = inb(REG_CMD(dev->base));
                if (st == 0 || (st & STS_ERR)) {
                    if (attempt == 0 && dev->drive_bit != 0x00) {
                        uart_printf("[Nova] ATA: dev%d retry IDENTIFY "
                                    "(st=0x%02x)\r\n", devno, st);
                        outb(REG_DRIVE(dev->base),
                             (uint8_t)(DRIVE_SEL_LBA | dev->drive_bit));
                        io_wait();
                        continue;
                    }
                    uart_printf("[Nova] ATA: dev%d IDENTIFY rejected "
                                "st=0x%02x\r\n", devno, st);
                    break;
                }
                if (ata_wait_drq() != 0) {
                    uint8_t st2 = inb(REG_CMD(dev->base));
                    if (attempt == 0 && dev->drive_bit != 0x00) {
                        uart_printf("[Nova] ATA: dev%d retry no DRQ "
                                    "(st=0x%02x)\r\n", devno, st2);
                        outb(REG_DRIVE(dev->base),
                             (uint8_t)(DRIVE_SEL_LBA | dev->drive_bit));
                        io_wait();
                        continue;
                    }
                    uart_printf("[Nova] ATA: dev%d IDENTIFY no DRQ "
                                "(st=0x%02x)\r\n", devno, st2);
                    break;
                }

                for (int i = 0; i < 256; i++)
                    id[i] = inw(REG_DATA(dev->base));

                dev->sector_count = ((uint64_t)id[61] << 16) | id[60];

                for (int i = 0; i < 40; i += 2) {
                    dev->model[i]     = (char)((id[27 + i / 2] >> 8) & 0xFF);
                    dev->model[i + 1] = (char)(id[27 + i / 2] & 0xFF);
                }
                dev->model[40] = '\0';
                for (int i = 39; i >= 0 && dev->model[i] == ' '; i--)
                    dev->model[i] = '\0';

                dev->valid = 1;
                uart_printf("[Nova] ATA: dev%d IDENTIFY ok, model='%s', "
                            "sectors=%llu (%llu MB)\r\n",
                            devno, dev->model,
                            (unsigned long long)dev->sector_count,
                            (unsigned long long)(dev->sector_count / 2048));
                dev_count++;
                done = 1;
            }
        }
    }

    if (dev_count == 0)
        return -ENODEV;
    cur_dev = &devs[0];
    return 0;
}

/* 发送 LBA28 读/写命令 */
static int ata_cmd_rw(uint64_t lba, uint16_t count, uint8_t cmd)
{
    if (lba >= (1u << 28) || count == 0 || count > 256)
        return -EINVAL;
    if (ata_wait_not_busy() != 0)
        return -EIO;

    outb(REG_SECTCOUNT(cur_dev->base), (uint8_t)(count & 0xFF));
    outb(REG_LBA_LOW(cur_dev->base),   (uint8_t)(lba & 0xFF));
    outb(REG_LBA_MID(cur_dev->base),   (uint8_t)((lba >> 8) & 0xFF));
    outb(REG_LBA_HIGH(cur_dev->base),  (uint8_t)((lba >> 16) & 0xFF));
    outb(REG_DRIVE(cur_dev->base),
         (uint8_t)(DRIVE_SEL_LBA | cur_dev->drive_bit | ((lba >> 24) & 0x0F)));
    outb(REG_CMD(cur_dev->base), cmd);
    return 0;
}

int ata_read_sectors(uint64_t lba, uint16_t count, void *buf)
{
    int err = ata_cmd_rw(lba, count, CMD_READ_SECT);
    if (err != 0)
        return err;

    uint16_t *p = buf;
    for (uint16_t s = 0; s < count; s++) {
        if (ata_wait_drq() != 0)
            return -EIO;
        for (int i = 0; i < 256; i++)
            *p++ = inw(REG_DATA(cur_dev->base));
    }
    if (ata_wait_not_busy() != 0)
        return -EIO;
    return 0;
}

/* 写扇区：超时/失败自动重发整条命令（最多 3 次）。
 * TCG 环境下主循环偶发卡顿会导致状态轮询超时，但设备稍后会完成；
 * 重发幂等（同一 LBA 同一数据），把偶发 -EIO 变成自愈。 */
int ata_write_sectors(uint64_t lba, uint16_t count, const void *buf)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        int err = ata_cmd_rw(lba, count, CMD_WRITE_SECT);
        if (err != 0)
            return err;

        const uint16_t *p = buf;
        int failed = 0;
        for (uint16_t s = 0; s < count; s++) {
            if (ata_wait_drq() != 0) {
                failed = 1;
                break;
            }
            for (int i = 0; i < 256; i++)
                outw(REG_DATA(cur_dev->base), *p++);
            /* 等待写入完成（BSY 清零后再发下一扇区） */
            if (ata_wait_not_busy() != 0) {
                failed = 1;
                break;
            }
        }
        if (!failed)
            return 0;
        uart_printf("[Nova] ATA: write retry %d (lba %llu n %u)\r\n",
                    attempt, (unsigned long long)lba, (unsigned)count);
    }
    return -EIO;
}
