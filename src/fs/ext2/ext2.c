/* kernel/ext2/ext2.c - Nova OS 阶段十六：ext2 只读文件系统实现
 *
 * 依赖：ATA（ata_read_sectors）按扇区读取分区；kmalloc 作块缓存。
 *
 * 布局（分区内，块大小 1024 时）：
 *   块 0：引导区（保留）
 *   偏移 1024：超级块（1024 字节）
 *   块 2：块组描述符表
 *   块 N：块位图 / inode 位图 / inode 表 / 数据块
 */
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "ext2.h"
#include "../../drivers/ata/ata.h"
#include "../vfs.h"
#include "../../core/mm/kmalloc.h"
#include "../../lib/uart.h"

/* ---- 磁盘结构（小端） ---- */

struct ext2_super {
    uint32_t inodes_count;       /* 0  */
    uint32_t blocks_count;       /* 4  */
    uint32_t r_blocks_count;     /* 8  */
    uint32_t free_blocks_count;  /* 12 */
    uint32_t free_inodes_count;  /* 16 */
    uint32_t first_data_block;   /* 20 */
    uint32_t log_block_size;     /* 24 */
    uint32_t log_frag_size;      /* 28 */
    uint32_t blocks_per_group;   /* 32 */
    uint32_t frags_per_group;    /* 36 */
    uint32_t inodes_per_group;   /* 40 */
    uint32_t mtime;              /* 44 */
    uint32_t wtime;              /* 48 */
    uint16_t mnt_count;          /* 52 */
    uint16_t max_mnt_count;      /* 54 */
    uint16_t magic;              /* 56 */
    uint16_t state;              /* 58 */
    uint16_t errors;             /* 60 */
    uint16_t minor_rev_level;    /* 62 */
    uint32_t lastcheck;          /* 64 */
    uint32_t checkinterval;      /* 68 */
    uint32_t creator_os;         /* 72 */
    uint32_t rev_level;          /* 76 */
    uint16_t def_resuid;         /* 80 */
    uint16_t def_resgid;         /* 82 */
    uint32_t first_ino;          /* 84 */
    uint16_t inode_size;         /* 88 */
    uint16_t block_group_nr;     /* 90 */
    uint32_t feature_compat;     /* 92 */
    uint32_t feature_incompat;   /* 96 */
    uint32_t feature_ro_compat;  /* 100 */
    uint8_t  uuid[16];           /* 104 */
    char     volume_name[16];    /* 120 */
    char     last_mounted[64];   /* 136 */
    uint32_t algo_bitmap;        /* 200 */
    uint8_t  prealloc_blocks;    /* 204 */
    uint8_t  prealloc_dir_blocks;/* 205 */
    uint16_t reserved_gdt_blocks;/* 206 */
    uint8_t  journal_uuid[16];   /* 208 */
    uint32_t journal_inum;       /* 224 */
    uint32_t journal_dev;        /* 228 */
    uint32_t last_orphan;        /* 232 */
    uint32_t hash_seed[4];       /* 236 */
    uint8_t  def_hash_version;   /* 252 */
    uint8_t  jnl_backup_type;    /* 253 */
    uint16_t desc_size;          /* 254 */
    uint32_t default_mount_opts; /* 256 */
    uint32_t first_meta_bg;      /* 260 */
    uint32_t mkfs_time;          /* 264 */
    uint32_t jnl_blocks[17];     /* 268 */
    uint32_t blocks_count_hi;    /* 336 */
    uint32_t r_blocks_count_hi;  /* 340 */
    uint32_t free_blocks_hi;     /* 344 */
    uint16_t min_extra_isize;    /* 348 */
    uint16_t want_extra_isize;   /* 350 */
    uint32_t flags;              /* 352 */
    uint16_t raid_stride;        /* 356 */
    uint16_t mmp_interval;       /* 358 */
    uint64_t mmp_block;          /* 360 */
    uint32_t raid_stripe_width;  /* 368 */
    uint8_t  log_groups_per_flex;/* 372 */
    uint8_t  checksum_type;      /* 373 */
    uint8_t  reserved_pad;       /* 374 */
    uint8_t  log_blocks_per_group;/* 375 */
    uint16_t reserved_pad2;      /* 376 */
    uint32_t kbytes_written;     /* 378 */
} __attribute__((packed));

struct ext2_bgd {
    uint32_t block_bitmap;       /* 0  */
    uint32_t inode_bitmap;       /* 4  */
    uint32_t inode_table;        /* 8  */
    uint16_t free_blocks_count;  /* 12 */
    uint16_t free_inodes_count;  /* 14 */
    uint16_t used_dirs_count;    /* 16 */
    uint16_t pad;                /* 18 */
    uint32_t reserved[3];        /* 20 */
} __attribute__((packed));

struct ext2_dirent {
    uint32_t inode;              /* 0  */
    uint16_t rec_len;            /* 4  */
    uint8_t  name_len;           /* 6  */
    uint8_t  file_type;          /* 7  */
    char     name[];             /* 8  */
} __attribute__((packed));

/* ---- 模块状态（多实例） ---- */

static struct ext2_fs fs_array[EXT2_MAX_FS];
static struct ext2_fs *cur;          /* 当前操作槽位（ext2_mount_slot 设置） */

/* 内部块读取使用 cur 指向的实例 */

/* ---- 底层块读取 ---- */

/* 读取分区内的一个块到 buf（块大小 1024/2048/4096 -> 2/4/8 扇区） */
static int ext2_read_block(uint32_t blkno, void *buf)
{
    uint32_t sectors = cur->block_size / 512;
    return ata_read_sectors(cur->part_lba + (uint64_t)blkno * sectors,
                            sectors, buf);
}

/* ---- 公共接口 ---- */

struct ext2_fs *ext2_fs_get(int slot)
{
    if (slot < 0 || slot >= EXT2_MAX_FS || !fs_array[slot].valid)
        return NULL;
    return &fs_array[slot];
}

int ext2_select(int slot)
{
    if (slot < 0 || slot >= EXT2_MAX_FS || !fs_array[slot].valid)
        return -ENODEV;
    /* 每次切换槽位都重新选中所属 ATA 设备，保证多盘读取正确 */
    if (ata_select_device(fs_array[slot].ata_dev) != 0)
        return -EIO;
    cur = &fs_array[slot];
    return 0;
}

uint32_t ext2_block_size_s(int slot)   { return fs_array[slot].block_size; }
uint32_t ext2_total_blocks_s(int slot) { return fs_array[slot].total_bytes / fs_array[slot].block_size; }
uint64_t ext2_total_bytes_s(int slot)  { return fs_array[slot].total_bytes; }
uint64_t ext2_free_bytes_s(int slot)   { return fs_array[slot].free_bytes; }

int ext2_mount_slot(int slot, uint32_t start_lba, int ata_dev)
{
    uint8_t buf[1024];
    struct ext2_super sb;

    if (slot < 0 || slot >= EXT2_MAX_FS)
        return -EINVAL;
    if (ata_dev < 0 || ata_select_device(ata_dev) != 0)
        return -ENODEV;
    if (ata_read_sectors(start_lba + 2, 2, buf) != 0)
        return -EIO;
    memcpy(&sb, buf, sizeof(struct ext2_super));

    if (sb.magic != EXT2_MAGIC)
        return -ENODEV;
    if (sb.log_block_size > 2)
        return -EINVAL;                       /* 块最大 4096 */

    struct ext2_fs *fs = &fs_array[slot];
    fs->part_lba = start_lba;
    fs->ata_dev = ata_dev;
    fs->block_size = 1024u << sb.log_block_size;
    fs->inode_size = (sb.inode_size != 0) ? sb.inode_size : 128;
    fs->inodes_per_group = sb.inodes_per_group;
    fs->blocks_per_group = sb.blocks_per_group;
    fs->free_blocks = sb.free_blocks_count;
    fs->free_inodes = sb.free_inodes_count;
    fs->total_blocks = sb.blocks_count;
    fs->total_bytes = (uint64_t)sb.blocks_count * fs->block_size;
    fs->free_bytes = (uint64_t)sb.free_blocks_count * fs->block_size;
    fs->valid = 1;
    cur = fs;                                 /* 后续 read 操作此槽位 */

    uart_printf("[Nova] ext2: super ok slot=%d (magic=0x%x blocks=%u inodes=%u blk=%u free=%u)\r\n",
                slot, (unsigned)sb.magic, (unsigned)sb.blocks_count,
                (unsigned)sb.inodes_count, (unsigned)fs->block_size,
                (unsigned)sb.free_blocks_count);
    return 0;
}

/* 兼容旧 API：挂载到槽位 0 并设为当前（阶段十六/十七测试用），
 * 设备取当前已选中的 ATA 设备。 */
int ext2_mount(uint32_t start_lba)
{
    return ext2_mount_slot(0, start_lba, ata_current_index());
}

/* 读取块组描述符表并返回第 g 组 */
static int ext2_read_bgd(uint32_t g, struct ext2_bgd *out)
{
    uint32_t bgd_blk;
    uint32_t off;

    if (cur->block_size == 1024) {
        bgd_blk = 2;                          /* 超级块后第一块 */
        off = g * sizeof(struct ext2_bgd);
    } else {
        bgd_blk = 1;
        off = g * sizeof(struct ext2_bgd);
    }

    uint8_t *blk = kmalloc(cur->block_size);
    if (blk == NULL)
        return -ENOMEM;
    int err = ext2_read_block(bgd_blk + off / cur->block_size, blk);
    if (err != 0) {
        kfree(blk);
        return err;
    }
    memcpy(out, blk + off % cur->block_size, sizeof(struct ext2_bgd));
    kfree(blk);
    return 0;
}

/* 调整组 g 的 bgd 计数（dirs: used_dirs；fi: free_inodes；fb: free_blocks） */
static int ext2_write_block(uint32_t blkno, const void *buf); /* 前向声明 */
static int ext2_bgd_patch(uint32_t g, int dirs_delta, int fi_delta,
                          int fb_delta)
{
    struct ext2_bgd bgd;
    int err = ext2_read_bgd(g, &bgd);
    if (err != 0)
        return err;
    int vd = (int)bgd.used_dirs_count + dirs_delta;
    if (vd < 0)
        vd = 0;
    bgd.used_dirs_count = (uint16_t)vd;
    int vf = (int)bgd.free_inodes_count + fi_delta;
    if (vf < 0)
        vf = 0;
    bgd.free_inodes_count = (uint16_t)vf;
    int vb = (int)bgd.free_blocks_count + fb_delta;
    if (vb < 0)
        vb = 0;
    bgd.free_blocks_count = (uint16_t)vb;

    uint32_t bgd_blk = 2 + (g * sizeof(struct ext2_bgd)) / cur->block_size;
    uint32_t off = (g * sizeof(struct ext2_bgd)) % cur->block_size;
    uint8_t *blk = kmalloc(cur->block_size);
    if (blk == NULL)
        return -ENOMEM;
    err = ext2_read_block(bgd_blk, blk);
    if (err == 0) {
        memcpy(blk + off, &bgd, sizeof(bgd));
        err = ext2_write_block(bgd_blk, blk);
    }
    kfree(blk);
    return err;
}

static int ext2_bgd_dirs_delta(uint32_t g, int delta)
{
    return ext2_bgd_patch(g, delta, 0, 0);
}

int ext2_read_inode(uint32_t ino, struct ext2_inode *out)
{
    if (ino == 0)
        return -EINVAL;
    if (cur == NULL)               /* 未挂载任何卷（防御：NULL cur 除零） */
        return -ENODEV;
    uint32_t g = (ino - 1) / cur->inodes_per_group;
    uint32_t idx = (ino - 1) % cur->inodes_per_group;

    struct ext2_bgd bgd;
    int err = ext2_read_bgd(g, &bgd);
    if (err != 0)
        return err;

    /* inode 表块 + 块内偏移 */
    uint32_t inode_blk = bgd.inode_table + (idx * cur->inode_size) / cur->block_size;
    uint32_t inode_off = (idx * cur->inode_size) % cur->block_size;

    uint8_t *blk = kmalloc(cur->block_size);
    if (blk == NULL)
        return -ENOMEM;
    err = ext2_read_block(inode_blk, blk);
    if (err == 0)
        memcpy(out, blk + inode_off, sizeof(struct ext2_inode));
    kfree(blk);
    return err;
}

/* 返回文件第 blk 个数据块的块号（直接 + 单间接） */
static int ext2_block_lookup(const struct ext2_inode *in, uint32_t idx,
                             uint32_t *blkno)
{
    if (idx < EXT2_NDIR_BLOCKS) {
        *blkno = in->block[idx];
        return 0;
    }
    if (idx < EXT2_NDIR_BLOCKS + cur->block_size / 4) {
        uint8_t *tbl = kmalloc(cur->block_size);
        if (tbl == NULL)
            return -ENOMEM;
        int err = ext2_read_block(in->block[EXT2_IND_BLOCK], tbl);
        if (err == 0)
            *blkno = ((uint32_t *)tbl)[idx - EXT2_NDIR_BLOCKS];
        kfree(tbl);
        return err;
    }
    return -EFBIG;                           /* 超出直接+单间接范围 */
}

int ext2_read_file(const struct ext2_inode *in, uint64_t off,
                   void *buf, uint64_t len, uint64_t *got)
{
    if (cur == NULL)
        return -ENODEV;
    uint64_t size = in->size;
    if (off >= size || len == 0) {
        if (got)
            *got = 0;
        return 0;
    }
    if (len > size - off)
        len = size - off;

    uint8_t *blk = kmalloc(cur->block_size);
    if (blk == NULL)
        return -ENOMEM;

    uint64_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t blk_idx = (uint32_t)(pos / cur->block_size);
        uint32_t blk_off = (uint32_t)(pos % cur->block_size);

        uint32_t blkno;
        int err = ext2_block_lookup(in, blk_idx, &blkno);
        if (err != 0) {
            kfree(blk);
            return err;
        }
        err = ext2_read_block(blkno, blk);
        if (err != 0) {
            kfree(blk);
            return err;
        }

        uint64_t chunk = cur->block_size - blk_off;
        if (chunk > len - done)
            chunk = len - done;
        memcpy((uint8_t *)buf + done, blk + blk_off, (size_t)chunk);
        done += chunk;
    }
    kfree(blk);
    if (got)
        *got = done;
    return 0;
}

/* 枚举目录：按块扫描目录项 */
int ext2_read_dir(const struct ext2_inode *dir, uint64_t idx,
                  char *name, uint32_t *type, uint32_t *ino_out)
{
    if (cur == NULL)
        return -ENODEV;
    uint8_t *blk = kmalloc(cur->block_size);
    if (blk == NULL)
        return -ENOMEM;

    uint64_t nblocks = (dir->size + cur->block_size - 1) / cur->block_size;
    uint64_t seen = 0;

    for (uint64_t b = 0; b < nblocks; b++) {
        uint32_t blkno;
        int err = ext2_block_lookup(dir, (uint32_t)b, &blkno);
        if (err != 0) {
            kfree(blk);
            return err;
        }
        err = ext2_read_block(blkno, blk);
        if (err != 0) {
            kfree(blk);
            return err;
        }
        for (uint32_t off = 0; off < cur->block_size;) {
            struct ext2_dirent *de = (struct ext2_dirent *)(blk + off);
            if (de->rec_len == 0)
                break;
            if (de->inode != 0) {
                if (seen == idx) {
                    uint32_t nl = de->name_len;
                    if (nl > 63)
                        nl = 63;
                    memcpy(name, de->name, nl);
                    name[nl] = '\0';
                    *type = (de->file_type == EXT2_FT_DIR) ? 2u : 1u;
                    if (ino_out)
                        *ino_out = de->inode;
                    kfree(blk);
                    return 0;
                }
                seen++;
            }
            off += de->rec_len;
        }
    }
    kfree(blk);
    return 1;                                 /* 枚举完毕 */
}

uint32_t ext2_lookup(const struct ext2_inode *dir, const char *name)
{
    if (cur == NULL)
        return 0;
    uint8_t *blk = kmalloc(cur->block_size);
    if (blk == NULL)
        return 0;

    uint64_t nblocks = (dir->size + cur->block_size - 1) / cur->block_size;
    for (uint64_t b = 0; b < nblocks; b++) {
        uint32_t blkno;
        if (ext2_block_lookup(dir, (uint32_t)b, &blkno) != 0)
            break;
        if (ext2_read_block(blkno, blk) != 0)
            break;
        for (uint32_t off = 0; off < cur->block_size;) {
            struct ext2_dirent *de = (struct ext2_dirent *)(blk + off);
            if (de->rec_len == 0)
                break;
            if (de->inode != 0 && de->name_len == strlen(name) &&
                memcmp(de->name, name, de->name_len) == 0) {
                uint32_t ino = de->inode;
                kfree(blk);
                return ino;
            }
            off += de->rec_len;
        }
    }
    kfree(blk);
    return 0;
}

/* ================================================================== */
/* 写支持（持久化：创建/删除/写/截断）                                 */
/* 分配策略：位图立即写回（崩溃一致性不保证，但位图始终真实）；          */
/* 超级块空闲计数在内存跟踪，每次顶层操作结束时同步一次。                */
/* ================================================================== */

/* 写一个块 */
static int ext2_write_block(uint32_t blkno, const void *buf)
{
    uint32_t sectors = cur->block_size / 512;
    return ata_write_sectors(cur->part_lba + (uint64_t)blkno * sectors,
                             sectors, buf);
}

/* 写 inode（布局与 ext2_read_inode 一致） */
static int ext2_write_inode(uint32_t ino, const struct ext2_inode *in)
{
    if (ino == 0)
        return -EINVAL;
    uint32_t g = (ino - 1) / cur->inodes_per_group;
    uint32_t idx = (ino - 1) % cur->inodes_per_group;

    struct ext2_bgd bgd;
    int err = ext2_read_bgd(g, &bgd);
    if (err != 0)
        return err;

    uint32_t inode_blk = bgd.inode_table +
                         (idx * cur->inode_size) / cur->block_size;
    uint32_t inode_off = (idx * cur->inode_size) % cur->block_size;

    uint8_t *blk = kmalloc(cur->block_size);
    if (blk == NULL)
        return -ENOMEM;
    err = ext2_read_block(inode_blk, blk);
    if (err == 0) {
        memcpy(blk + inode_off, in, sizeof(struct ext2_inode));
        err = ext2_write_block(inode_blk, blk);
    }
    kfree(blk);
    return err;
}

/* 同步超级块空闲计数（2 扇区读 + 2 扇区写） */
static int ext2_sync_super(void)
{
    uint8_t buf[1024];
    int err = ata_read_sectors(cur->part_lba + 2, 2, buf);
    if (err != 0)
        return err;
    struct ext2_super *sb = (struct ext2_super *)buf;
    sb->free_blocks_count = cur->free_blocks;
    sb->free_inodes_count = cur->free_inodes;
    return ata_write_sectors(cur->part_lba + 2, 2, buf);
}

/* 在组 g 的块位图中找空闲块并置位；成功置 *blkno */
static int ext2_alloc_block_in_group(uint32_t g, uint32_t *blkno)
{
    struct ext2_bgd bgd;
    int err = ext2_read_bgd(g, &bgd);
    if (err != 0)
        return err;
    uint8_t *bm = kmalloc(cur->block_size);
    if (bm == NULL)
        return -ENOMEM;
    err = ext2_read_block(bgd.block_bitmap, bm);
    if (err != 0) {
        kfree(bm);
        return err;
    }
    uint32_t limit = cur->blocks_per_group;
    /* 末组位上限：最后有效块 = total-1，位 i 对应块 1+g*BPG+i */
    if (limit > cur->total_blocks - 1 - g * cur->blocks_per_group)
        limit = cur->total_blocks - 1 - g * cur->blocks_per_group;
    for (uint32_t i = 0; i < limit; i++) {
        if (!(bm[i / 8] & (uint8_t)(1u << (i % 8)))) {
            bm[i / 8] |= (uint8_t)(1u << (i % 8));
            err = ext2_write_block(bgd.block_bitmap, bm);
            kfree(bm);
            if (err != 0)
                return err;
            /* 标准位约定：位 i 对应块 1+g*BPG+i（块 0 不在文件系统内） */
            *blkno = 1 + g * cur->blocks_per_group + i;
            return 0;
        }
    }
    kfree(bm);
    return -ENOSPC;
}

/* 分配一个数据块；成功置 *blkno（0 保留给空洞） */
static int ext2_alloc_block(uint32_t *blkno)
{
    uint32_t groups = (cur->total_blocks + cur->blocks_per_group - 1) /
                      cur->blocks_per_group;
    for (uint32_t g = 0; g < groups; g++) {
        int err = ext2_alloc_block_in_group(g, blkno);
        if (err == 0) {
            cur->free_blocks--;
            ext2_bgd_patch(g, 0, 0, -1);   /* bgd 组内空闲块计数 */
            return 0;
        }
        if (err != -ENOSPC)
            return err;
    }
    return -ENOSPC;
}

/* 释放数据块（位图清位；位 i 对应块 1+g*BPG+i） */
static int ext2_free_block(uint32_t blkno)
{
    if (blkno == 0)
        return 0;
    uint32_t b = blkno - 1;
    uint32_t g = b / cur->blocks_per_group;
    uint32_t i = b % cur->blocks_per_group;
    struct ext2_bgd bgd;
    int err = ext2_read_bgd(g, &bgd);
    if (err != 0)
        return err;
    uint8_t *bm = kmalloc(cur->block_size);
    if (bm == NULL)
        return -ENOMEM;
    err = ext2_read_block(bgd.block_bitmap, bm);
    if (err == 0) {
        bm[i / 8] &= (uint8_t)~(uint8_t)(1u << (i % 8));
        err = ext2_write_block(bgd.block_bitmap, bm);
        if (err == 0) {
            cur->free_blocks++;
            ext2_bgd_patch(g, 0, 0, +1);
        }
    }
    kfree(bm);
    return err;
}

/* 分配 inode；成功置 *ino_out（>= 11） */
static int ext2_alloc_inode(uint32_t *ino_out)
{
    uint32_t groups = (cur->total_blocks + cur->blocks_per_group - 1) /
                      cur->blocks_per_group;
    for (uint32_t g = 0; g < groups; g++) {
        struct ext2_bgd bgd;
        if (ext2_read_bgd(g, &bgd) != 0)
            return -EIO;
        uint8_t *bm = kmalloc(cur->block_size);
        if (bm == NULL)
            return -ENOMEM;
        int err = ext2_read_block(bgd.inode_bitmap, bm);
        if (err != 0) {
            kfree(bm);
            return err;
        }
        for (uint32_t i = 0; i < cur->inodes_per_group; i++) {
            if (!(bm[i / 8] & (uint8_t)(1u << (i % 8)))) {
                bm[i / 8] |= (uint8_t)(1u << (i % 8));
                err = ext2_write_block(bgd.inode_bitmap, bm);
                kfree(bm);
                if (err != 0)
                    return err;
                *ino_out = g * cur->inodes_per_group + i + 1;
                cur->free_inodes--;
                /* bgd 组内空闲 inode 计数（e2fsck pass 5 核对） */
                ext2_bgd_patch(g, 0, -1, 0);
                return 0;
            }
        }
        kfree(bm);
    }
    return -ENOSPC;
}

/* 释放 inode（位图清位 + 表槽清零：e2fsck 的 used_map 按表内容判断，
 * 槽位残留 mode 会把已删除 inode 当 unattached 报错） */
static int ext2_free_inode(uint32_t ino)
{
    if (ino == 0)
        return 0;
    uint32_t g = (ino - 1) / cur->inodes_per_group;
    uint32_t i = (ino - 1) % cur->inodes_per_group;
    struct ext2_bgd bgd;
    int err = ext2_read_bgd(g, &bgd);
    if (err != 0)
        return err;
    uint8_t *bm = kmalloc(cur->block_size);
    if (bm == NULL)
        return -ENOMEM;
    err = ext2_read_block(bgd.inode_bitmap, bm);
    if (err == 0) {
        bm[i / 8] &= (uint8_t)~(uint8_t)(1u << (i % 8));
        err = ext2_write_block(bgd.inode_bitmap, bm);
        if (err == 0) {
            cur->free_inodes++;
            ext2_bgd_patch(g, 0, +1, 0);
        }
    }
    kfree(bm);
    if (err != 0)
        return err;

    /* 清零 inode 表槽位（读-改-写） */
    uint32_t inode_blk = bgd.inode_table +
                         (i * cur->inode_size) / cur->block_size;
    uint32_t inode_off = (i * cur->inode_size) % cur->block_size;
    uint8_t *iblk = kmalloc(cur->block_size);
    if (iblk == NULL)
        return -ENOMEM;
    err = ext2_read_block(inode_blk, iblk);
    if (err == 0) {
        memset(iblk + inode_off, 0, cur->inode_size);
        err = ext2_write_block(inode_blk, iblk);
    }
    kfree(iblk);
    return err;
}

/* 设置 inode 第 idx 个数据块指针（直接/单间接；自动分配间接表）。
 * 调用者需在最后写回 inode。 */
static int ext2_set_block_ptr(struct ext2_inode *in, uint32_t idx,
                              uint32_t blkno)
{
    if (idx < EXT2_NDIR_BLOCKS) {
        in->block[idx] = blkno;
        return 0;
    }
    if (idx < EXT2_NDIR_BLOCKS + cur->block_size / 4) {
        if (in->block[EXT2_IND_BLOCK] == 0) {
            uint32_t tbl;
            int err = ext2_alloc_block(&tbl);
            if (err != 0)
                return err;
            in->block[EXT2_IND_BLOCK] = tbl;
            uint8_t *z = kcalloc(1, cur->block_size);
            if (z == NULL)
                return -ENOMEM;
            err = ext2_write_block(tbl, z);
            kfree(z);
            if (err != 0)
                return err;
        }
        uint8_t *tbl = kmalloc(cur->block_size);
        if (tbl == NULL)
            return -ENOMEM;
        int err = ext2_read_block(in->block[EXT2_IND_BLOCK], tbl);
        if (err == 0) {
            ((uint32_t *)tbl)[idx - EXT2_NDIR_BLOCKS] = blkno;
            err = ext2_write_block(in->block[EXT2_IND_BLOCK], tbl);
        }
        kfree(tbl);
        return err;
    }
    return -EFBIG;                       /* 超出直接+单间接 */
}

/* 释放文件全部数据块（直接 + 单间接表及其数据）；清空 inode 指针 */
static int ext2_free_all_blocks(struct ext2_inode *in)
{
    uint32_t nblocks = (in->size + cur->block_size - 1) / cur->block_size;
    uint32_t direct = nblocks < EXT2_NDIR_BLOCKS ? nblocks : EXT2_NDIR_BLOCKS;
    for (uint32_t i = 0; i < direct; i++) {
        if (in->block[i] != 0) {
            ext2_free_block(in->block[i]);
            in->block[i] = 0;
        }
    }
    if (nblocks > EXT2_NDIR_BLOCKS && in->block[EXT2_IND_BLOCK] != 0) {
        uint8_t *tbl = kmalloc(cur->block_size);
        if (tbl == NULL)
            return -ENOMEM;
        int err = ext2_read_block(in->block[EXT2_IND_BLOCK], tbl);
        if (err == 0) {
            uint32_t nind = nblocks - EXT2_NDIR_BLOCKS;
            if (nind > cur->block_size / 4)
                nind = cur->block_size / 4;
            for (uint32_t i = 0; i < nind; i++) {
                if (((uint32_t *)tbl)[i] != 0)
                    ext2_free_block(((uint32_t *)tbl)[i]);
            }
        }
        kfree(tbl);
        ext2_free_block(in->block[EXT2_IND_BLOCK]);
        in->block[EXT2_IND_BLOCK] = 0;
    }
    in->blocks = 0;
    return 0;
}

/* 在目录下追加目录项（不查重名；dir_ino 为目录 inode） */
static int ext2_dir_add(uint32_t dir_ino, const char *name, uint32_t child_ino,
                        uint8_t ftype)
{
    uint32_t nl = (uint32_t)strlen(name);
    if (nl == 0 || nl > 60)
        return -EINVAL;
    uint32_t need = 8 + ((nl + 3) & ~3u);
    if (need < 12)
        need = 12;

    struct ext2_inode dir;
    int err = ext2_read_inode(dir_ino, &dir);
    if (err != 0)
        return err;
    if ((dir.mode & 0xF000u) != 0x4000u)
        return -ENOTDIR;

    uint8_t *blk = kmalloc(cur->block_size);
    if (blk == NULL)
        return -ENOMEM;

    /* 1) 找现有块中末尾项的 slack 空间 */
    uint32_t nblocks = (dir.size + cur->block_size - 1) / cur->block_size;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blkno;
        if (ext2_block_lookup(&dir, b, &blkno) != 0)
            break;
        if (ext2_read_block(blkno, blk) != 0)
            break;
        uint32_t off = 0;
        while (off < cur->block_size) {
            struct ext2_dirent *de = (struct ext2_dirent *)(blk + off);
            if (de->rec_len == 0)
                break;
            if (off + de->rec_len >= cur->block_size) {
                /* 块末项：slack = rec_len - 实际占用 */
                uint32_t used = 8 + ((de->name_len + 3) & ~3u);
                if (used < 12)
                    used = 12;
                uint32_t slack = de->rec_len - used;
                if (slack >= need) {
                    struct ext2_dirent *nd =
                        (struct ext2_dirent *)(blk + off + used);
                    nd->inode = child_ino;
                    nd->rec_len = (uint16_t)slack;
                    nd->name_len = (uint8_t)nl;
                    nd->file_type = ftype;
                    memcpy(nd->name, name, nl);
                    de->rec_len = (uint16_t)used;
                    err = ext2_write_block(blkno, blk);
                    kfree(blk);
                    return err;
                }
                break;                   /* 该块无空间，试下一块 */
            }
            off += de->rec_len;
        }
    }

    /* 2) 追加新块 */
    uint32_t nblk;
    err = ext2_alloc_block(&nblk);
    if (err != 0) {
        kfree(blk);
        return err;
    }
    memset(blk, 0, cur->block_size);
    struct ext2_dirent *de = (struct ext2_dirent *)blk;
    de->inode = child_ino;
    de->rec_len = (uint16_t)cur->block_size;
    de->name_len = (uint8_t)nl;
    de->file_type = ftype;
    memcpy(de->name, name, nl);
    err = ext2_write_block(nblk, blk);
    kfree(blk);
    if (err != 0)
        return err;
    err = ext2_set_block_ptr(&dir, nblocks, nblk);
    if (err != 0)
        return err;
    dir.size += cur->block_size;
    dir.blocks += (uint32_t)(cur->block_size / 512);
    return ext2_write_inode(dir_ino, &dir);
}

/* 删除目录项：置 inode=0；若被删项是块末项，把它的空间并入前一项
 * 的 rec_len（避免留下 inode=0 的墓碑占位）。 */
static int ext2_dir_remove(uint32_t dir_ino, const char *name)
{
    struct ext2_inode dir;
    int err = ext2_read_inode(dir_ino, &dir);
    if (err != 0)
        return err;
    uint8_t *blk = kmalloc(cur->block_size);
    if (blk == NULL)
        return -ENOMEM;
    uint32_t nblocks = (dir.size + cur->block_size - 1) / cur->block_size;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blkno;
        if (ext2_block_lookup(&dir, b, &blkno) != 0)
            break;
        if (ext2_read_block(blkno, blk) != 0)
            break;
        uint32_t off = 0;
        struct ext2_dirent *prev = NULL;
        while (off < cur->block_size) {
            struct ext2_dirent *de = (struct ext2_dirent *)(blk + off);
            if (de->rec_len == 0)
                break;
            if (de->inode != 0 && de->name_len == strlen(name) &&
                memcmp(de->name, name, de->name_len) == 0) {
                if (off + de->rec_len >= cur->block_size) {
                    /* 块末项：并入前一项，空间立即回收 */
                    if (prev != NULL)
                        prev->rec_len += de->rec_len;
                    else
                        de->inode = 0;   /* 首项（不应发生）兜底 */
                } else {
                    de->inode = 0;
                }
                err = ext2_write_block(blkno, blk);
                kfree(blk);
                return err;
            }
            prev = de;
            off += de->rec_len;
        }
    }
    kfree(blk);
    return -ENOENT;
}

/* ---- 公开写接口 ---- */

int ext2_create(uint32_t dir_ino, const char *name, uint32_t type,
                uint32_t *ino_out)
{
    if (cur == NULL)
        return -ENODEV;
    if (name == NULL || ino_out == NULL || name[0] == '\0')
        return -EINVAL;

    struct ext2_inode dir;
    int err = ext2_read_inode(dir_ino, &dir);
    if (err != 0)
        return err;
    if (ext2_lookup(&dir, name) != 0)
        return -EEXIST;

    uint32_t ino;
    err = ext2_alloc_inode(&ino);
    if (err != 0)
        return err;

    struct ext2_inode in;
    memset(&in, 0, sizeof(in));
    if (type == VFS_TYPE_DIR) {
        in.mode = 0x41EDu;               /* dir | 0755 */
        in.links_count = 2;
        in.size = cur->block_size;
        uint32_t dblk;
        err = ext2_alloc_block(&dblk);
        if (err != 0) {
            ext2_free_inode(ino);
            return err;
        }
        in.block[0] = dblk;
        in.blocks = (uint32_t)(cur->block_size / 512);
        uint8_t *blk = kcalloc(1, cur->block_size);
        if (blk == NULL) {
            ext2_free_block(dblk);
            ext2_free_inode(ino);
            return -ENOMEM;
        }
        struct ext2_dirent *d1 = (struct ext2_dirent *)blk;
        d1->inode = ino;
        d1->rec_len = 12;
        d1->name_len = 1;
        d1->file_type = EXT2_FT_DIR;
        memcpy(d1->name, ".", 1);
        struct ext2_dirent *d2 = (struct ext2_dirent *)(blk + 12);
        d2->inode = dir_ino;
        d2->rec_len = (uint16_t)(cur->block_size - 12);
        d2->name_len = 2;
        d2->file_type = EXT2_FT_DIR;
        memcpy(d2->name, "..", 2);
        err = ext2_write_block(dblk, blk);
        kfree(blk);
        if (err != 0) {
            ext2_free_block(dblk);
            ext2_free_inode(ino);
            return err;
        }
        dir.links_count++;
        err = ext2_write_inode(dir_ino, &dir);
        if (err != 0)
            return err;
        /* 组目录计数（e2fsck pass 5 核对 used_dirs_count） */
        ext2_bgd_dirs_delta((ino - 1) / cur->inodes_per_group, +1);
    } else {
        in.mode = 0x81A4u;               /* reg | 0644 */
        in.links_count = 1;
        in.size = 0;
    }

    err = ext2_write_inode(ino, &in);
    if (err != 0) {
        ext2_free_inode(ino);
        return err;
    }
    err = ext2_dir_add(dir_ino, name, ino,
                       (type == VFS_TYPE_DIR) ? EXT2_FT_DIR : EXT2_FT_FILE);
    if (err != 0) {
        ext2_free_inode(ino);
        return err;
    }
    err = ext2_sync_super();
    if (err != 0)
        return err;
    *ino_out = ino;
    return 0;
}

int ext2_write_file(uint32_t ino, uint64_t off, const void *buf,
                    uint64_t len)
{
    if (cur == NULL)
        return -ENODEV;
    if (len == 0)
        return 0;

    struct ext2_inode in;
    int err = ext2_read_inode(ino, &in);
    if (err != 0)
        return err;
    if ((in.mode & 0xF000u) != 0x8000u)
        return -EISDIR;

    uint64_t end = off + len;
    uint64_t b_start = off / cur->block_size;
    uint64_t b_end = (end + cur->block_size - 1) / cur->block_size;

    uint8_t *blk = kmalloc(cur->block_size);
    if (blk == NULL)
        return -ENOMEM;

    for (uint64_t bi = b_start; bi < b_end; bi++) {
        uint32_t blkno;
        int fresh = 0;
        err = ext2_block_lookup(&in, (uint32_t)bi, &blkno);
        if (err != 0) {
            kfree(blk);
            return err;
        }
        if (blkno == 0) {
            err = ext2_alloc_block(&blkno);
            if (err != 0) {
                kfree(blk);
                return err;
            }
            err = ext2_set_block_ptr(&in, (uint32_t)bi, blkno);
            if (err != 0) {
                kfree(blk);
                return err;
            }
            in.blocks += (uint32_t)(cur->block_size / 512);
            fresh = 1;
        }
        if (fresh)
            memset(blk, 0, cur->block_size);
        else {
            err = ext2_read_block(blkno, blk);
            if (err != 0) {
                kfree(blk);
                return err;
            }
        }

        uint64_t b_off = bi * cur->block_size;
        uint64_t lo = (off > b_off) ? (off - b_off) : 0;
        uint64_t hi = end - b_off;
        if (hi > cur->block_size)
            hi = cur->block_size;
        memcpy(blk + lo, (const uint8_t *)buf + (b_off + lo - off),
               (size_t)(hi - lo));
        err = ext2_write_block(blkno, blk);
        if (err != 0) {
            kfree(blk);
            return err;
        }
    }
    kfree(blk);

    if (end > in.size)
        in.size = (uint32_t)end;         /* 空洞已实化，无稀疏文件 */
    err = ext2_write_inode(ino, &in);
    if (err != 0)
        return err;
    return ext2_sync_super();
}

int ext2_truncate(uint32_t ino, uint64_t size)
{
    if (cur == NULL)
        return -ENODEV;
    struct ext2_inode in;
    int err = ext2_read_inode(ino, &in);
    if (err != 0)
        return err;
    uint32_t old_nb = (in.size + cur->block_size - 1) / cur->block_size;
    uint32_t new_nb = (size == 0) ? 0 :
        (uint32_t)((size + cur->block_size - 1) / cur->block_size);

    if (new_nb < old_nb) {
        for (uint32_t i = new_nb; i < old_nb; i++) {
            uint32_t blkno;
            if (ext2_block_lookup(&in, i, &blkno) != 0)
                break;
            if (blkno != 0) {
                ext2_free_block(blkno);
                if (in.blocks >= cur->block_size / 512)
                    in.blocks -= (uint32_t)(cur->block_size / 512);
            }
        }
        for (uint32_t i = new_nb; i < EXT2_NDIR_BLOCKS && i < old_nb; i++)
            in.block[i] = 0;
        if (old_nb > EXT2_NDIR_BLOCKS && in.block[EXT2_IND_BLOCK] != 0) {
            uint8_t *tbl = kmalloc(cur->block_size);
            if (tbl != NULL) {
                if (ext2_read_block(in.block[EXT2_IND_BLOCK], tbl) == 0) {
                    uint32_t base = EXT2_NDIR_BLOCKS;
                    uint32_t start = (new_nb > base) ? (new_nb - base) : 0;
                    uint32_t nind = old_nb - base;
                    if (nind > cur->block_size / 4)
                        nind = cur->block_size / 4;
                    for (uint32_t i = start; i < nind; i++)
                        ((uint32_t *)tbl)[i] = 0;
                    ext2_write_block(in.block[EXT2_IND_BLOCK], tbl);
                }
                kfree(tbl);
            }
            if (new_nb <= EXT2_NDIR_BLOCKS) {
                ext2_free_block(in.block[EXT2_IND_BLOCK]);
                in.block[EXT2_IND_BLOCK] = 0;
            }
        }
    }
    in.size = (uint32_t)size;
    err = ext2_write_inode(ino, &in);
    if (err != 0)
        return err;
    return ext2_sync_super();
}

int ext2_unlink(uint32_t dir_ino, const char *name)
{
    if (cur == NULL)
        return -ENODEV;
    struct ext2_inode dir;
    int err = ext2_read_inode(dir_ino, &dir);
    if (err != 0)
        return err;
    uint32_t ino = ext2_lookup(&dir, name);
    if (ino == 0)
        return -ENOENT;

    struct ext2_inode in;
    err = ext2_read_inode(ino, &in);
    if (err != 0)
        return err;

    if ((in.mode & 0xF000u) == 0x4000u) {
        /* 目录：仅允许删除空目录（只含 . 和 ..） */
        uint8_t *blk = kmalloc(cur->block_size);
        if (blk == NULL)
            return -ENOMEM;
        int empty = 1;
        uint32_t nblocks = (in.size + cur->block_size - 1) /
                           cur->block_size;
        for (uint32_t b = 0; b < nblocks && empty; b++) {
            uint32_t blkno;
            if (ext2_block_lookup(&in, b, &blkno) != 0)
                break;
            if (ext2_read_block(blkno, blk) != 0)
                break;
            for (uint32_t off = 0; off < cur->block_size;) {
                struct ext2_dirent *de = (struct ext2_dirent *)(blk + off);
                if (de->rec_len == 0)
                    break;
                if (de->inode != 0 &&
                    !(de->name_len == 1 && de->name[0] == '.') &&
                    !(de->name_len == 2 && de->name[0] == '.' &&
                      de->name[1] == '.')) {
                    empty = 0;
                    break;
                }
                off += de->rec_len;
            }
        }
        kfree(blk);
        if (!empty)
            return -ENOTEMPTY;
        if (dir.links_count > 2)
            dir.links_count--;
        err = ext2_write_inode(dir_ino, &dir);
        if (err != 0)
            return err;
        ext2_bgd_dirs_delta((ino - 1) / cur->inodes_per_group, -1);
    }

    err = ext2_free_all_blocks(&in);
    if (err != 0)
        return err;
    err = ext2_dir_remove(dir_ino, name);
    if (err != 0)
        return err;
    err = ext2_free_inode(ino);
    if (err != 0)
        return err;
    return ext2_sync_super();
}
