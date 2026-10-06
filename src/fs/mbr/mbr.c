/* kernel/mbr/mbr.c - Nova OS 阶段十五：MBR 分区表解析实现
 *
 * 依赖 ATA 驱动（ata_read_sectors）读取磁盘扇区 0。
 */
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "mbr.h"
#include "../../drivers/ata/ata.h"

int mbr_read(struct mbr *m)
{
    if (m == NULL)
        return -EINVAL;
    return ata_read_sectors(0, 1, m);
}

int mbr_validate(const struct mbr *m)
{
    if (m == NULL)
        return 0;
    return m->signature == MBR_SIGNATURE;
}

int mbr_count_parts(const struct mbr *m)
{
    if (m == NULL)
        return 0;
    int n = 0;
    for (int i = 0; i < 4; i++) {
        if (m->parts[i].type != 0)
            n++;
    }
    return n;
}

void mbr_build_sample(struct mbr *m)
{
    if (m == NULL)
        return;
    memset(m, 0, sizeof(*m));

    /* 分区 0：Linux（0x83），LBA 2048 起，64MB */
    m->parts[0].type      = 0x83u;
    m->parts[0].lba_start = 2048u;
    m->parts[0].sectors   = 131072u;

    /* 分区 1：FAT32 LBA（0x0C），LBA 133120 起，256MB */
    m->parts[1].type      = 0x0Cu;
    m->parts[1].lba_start = 133120u;
    m->parts[1].sectors   = 524288u;

    /* 分区 2：NTFS（0x07），LBA 657408 起，128MB */
    m->parts[2].type      = 0x07u;
    m->parts[2].lba_start = 657408u;
    m->parts[2].sectors   = 262144u;

    /* 分区 3：空（type=0） */

    m->signature = MBR_SIGNATURE;
}
