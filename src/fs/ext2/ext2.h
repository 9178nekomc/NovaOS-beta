/* kernel/ext2/ext2.h - Nova OS 阶段十六：ext2 只读文件系统接口
 *
 * 支持：超级块解析、块组描述符、inode 读取（直接/单间接块）、
 * 目录遍历、文件内容读取。挂载于 MBR 分区（传入起始 LBA）。
 */
#ifndef NOVA_EXT2_H
#define NOVA_EXT2_H

#include <stdint.h>

struct inode;   /* VFS inode（ext2fs.c 使用） */

#define EXT2_MAGIC      0xEF53u
#define EXT2_ROOT_INO   2u

#define EXT2_FT_FILE    1u
#define EXT2_FT_DIR     2u

#define EXT2_NDIR_BLOCKS 12u
#define EXT2_IND_BLOCK   12u   /* 单间接 */
#define EXT2_DIND_BLOCK  13u   /* 双间接（本阶段不展开） */
#define EXT2_TIND_BLOCK  14u

/* 最多同时挂载的 ext2 卷（多盘支持） */
#define EXT2_MAX_FS 4

/* ext2 卷实例（每盘一份状态） */
struct ext2_fs {
    uint32_t part_lba;             /* 分区起始 LBA */
    int      ata_dev;              /* 所属 ATA 设备索引（读盘前需选中） */
    uint32_t block_size;
    uint32_t inode_size;
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t total_blocks;         /* 卷总块数（写支持：块组遍历用） */
    uint64_t total_bytes;          /* 总容量 */
    uint64_t free_bytes;           /* 剩余容量 */
    uint32_t free_blocks;          /* 空闲块数（内存跟踪，同步回超级块） */
    uint32_t free_inodes;          /* 空闲 inode 数（写支持） */
    int valid;
};

/* 磁盘上的 ext2 inode（128 字节，小端） */
struct ext2_inode {
    uint16_t mode;              /* 0  */
    uint16_t uid;               /* 2  */
    uint32_t size;              /* 4  */
    uint32_t atime;             /* 8  */
    uint32_t ctime;             /* 12 */
    uint32_t mtime;             /* 16 */
    uint32_t dtime;             /* 20 */
    uint16_t gid;               /* 24 */
    uint16_t links_count;       /* 26 */
    uint32_t blocks;            /* 28（512 字节块数） */
    uint32_t flags;             /* 32 */
    uint32_t osd1;              /* 36 */
    uint32_t block[15];         /* 40：12 直接 + 单/双/三间接 */
    uint32_t generation;        /* 100 */
    uint32_t file_acl;          /* 104 */
    uint32_t dir_acl;           /* 108 */
    uint32_t faddr;             /* 112 */
    uint32_t osd2[3];           /* 116 */
} __attribute__((packed));

/* 只读挂载 ext2 分区到槽位 slot（超级块校验通过后返回 0）。
 * slot 0..EXT2_MAX_FS-1；part_lba 为分区起始扇区；ata_dev 为该
 * 分区所在的 ATA 设备索引（后续读取会自动选中该设备）。 */
int ext2_mount_slot(int slot, uint32_t part_lba, int ata_dev);

/* 兼容旧 API：挂载到槽位 0 并设为当前（阶段十六/十七测试用） */
int ext2_mount(uint32_t part_lba);

/* 返回槽位实例（未挂载返回 NULL） */
struct ext2_fs *ext2_fs_get(int slot);

/* 切换当前操作槽位（后续 read_* 作用于该卷）；返回 0 成功 */
int ext2_select(int slot);

/* 超级块基本信息（作用于指定槽位） */
uint32_t ext2_block_size_s(int slot);
uint32_t ext2_total_blocks_s(int slot);
uint64_t ext2_total_bytes_s(int slot);
uint64_t ext2_free_bytes_s(int slot);

/*
 * 读取 inode 到 out（inode_size 字节）。
 * 返回 0 成功；负 errno 失败。
 */
int ext2_read_inode(uint32_t ino, struct ext2_inode *out);

/*
 * 读取文件内容：从 off 起最多 len 字节，实际字节数存 *got。
 * 支持直接块（0-11）与单间接块（12）。
 */
int ext2_read_file(const struct ext2_inode *in, uint64_t off,
                   void *buf, uint64_t len, uint64_t *got);

/*
 * 枚举目录第 idx 项（0 起）：填 name 与 type（EXT2_FT_*）。
 * 返回 0 成功；1 枚举完毕；负 errno 失败。
 */
int ext2_read_dir(const struct ext2_inode *dir, uint64_t idx,
                  char *name, uint32_t *type, uint32_t *ino_out);

/* 查找目录中的文件，返回 inode 号（0 = 未找到） */
uint32_t ext2_lookup(const struct ext2_inode *dir, const char *name);

/* ---- 写支持（可持久化：创建/删除/写/截断） ---- */

/*
 * 在 dir_ino 目录下创建 name。
 * @type VFS_TYPE_FILE 或 VFS_TYPE_DIR（ext2fs.c 传入）
 * 成功置 *ino_out；失败负 errno。操作作用于当前槽位（cur）。
 */
int ext2_create(uint32_t dir_ino, const char *name, uint32_t type,
                uint32_t *ino_out);

/* 删除 dir_ino 目录下的 name（文件或空目录）；释放 inode 与数据块 */
int ext2_unlink(uint32_t dir_ino, const char *name);

/* 写文件：从 off 起写 len 字节（自动分配数据块，更新 inode size）。
 * 调用后 inode 已写回磁盘。 */
int ext2_write_file(uint32_t ino, uint64_t off, const void *buf,
                    uint64_t len);

/* 截断文件到 size（释放多余块并写回 inode） */
int ext2_truncate(uint32_t ino, uint64_t size);

/* ---- VFS 适配（ext2fs.c） ---- */

/* 挂载 ext2 槽位为 VFS 盘根；返回根 inode（NULL 失败） */
struct inode *ext2fs_mount_root(int slot);

#endif /* NOVA_EXT2_H */
