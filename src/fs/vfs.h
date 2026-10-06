/* kernel/fs/vfs.h - Nova OS 阶段十三：虚拟文件系统接口
 *
 * 极简 VFS：inode（文件/目录元数据 + 后端操作表 fs_ops）、
 * file（打开的文件描述，含读写位置）。tmpfs 为当前唯一后端，
 * 后续 ext2（阶段十六）实现同一 fs_ops 挂接。
 *
 * 路径：绝对路径，以 '/' 分隔，如 "/docs/notes.txt"。
 */
#ifndef NOVA_VFS_H
#define NOVA_VFS_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

typedef long ssize_t;   /* 内核无 sys/types.h，自行定义 */

#define VFS_MAX_NAME 64
#define VFS_PATH_MAX 256

enum {
    VFS_TYPE_FILE = 1,
    VFS_TYPE_DIR  = 2,
};

struct inode;
struct file;

/* 文件系统后端操作表（tmpfs 实现；ext2 后续实现） */
struct fs_ops {
    /* 在 dir 下创建 name（type = VFS_TYPE_*）；成功置 *out */
    int  (*create)(struct inode *dir, const char *name, uint32_t type,
                   struct inode **out);
    /* 在 dir 下查找 name */
    int  (*lookup)(struct inode *dir, const char *name, struct inode **out);
    /* 删除 dir 下的 name */
    int  (*unlink)(struct inode *dir, const char *name);
    /* 读 in 从 off 起 len 字节；返回实际字节数（负 errno 失败） */
    ssize_t (*read)(struct inode *in, void *buf, size_t len, uint64_t off);
    /* 写 in 从 off 起 len 字节；返回实际字节数（负 errno 失败） */
    ssize_t (*write)(struct inode *in, const void *buf, size_t len,
                     uint64_t off);
    /* 枚举 dir 第 idx 项（0 起）；成功填 name/type，越界返回 1 */
    int  (*readdir)(struct inode *dir, uint64_t idx, char *name,
                    uint32_t *type);
    /* 截断文件到 size */
    int  (*truncate)(struct inode *in, uint64_t size);
};

struct inode {
    uint64_t ino;            /* 文件序号（后端分配） */
    uint32_t type;           /* VFS_TYPE_* */
    uint64_t size;           /* 文件长度（字节） */
    struct fs_ops *ops;      /* 后端操作表 */
    void *private;           /* 后端私有数据（tmpfs_node *） */
};

struct file {
    struct inode *inode;
    uint64_t pos;            /* 当前读写位置 */
};

/* ---- VFS 顶层 API ---- */

#define VFS_MAX_DRIVES 4

/* 盘（卷）：盘符 + 根 inode + 容量信息 */
struct drive {
    char     letter;             /* 盘符 'C' 'D' 'E' 'F' */
    char     label[16];          /* 卷标 */
    struct inode *root;          /* 盘根 inode */
    uint64_t total_bytes;        /* 总容量（0 = 未知） */
    uint64_t free_bytes;         /* 剩余容量（0 = 未知） */
    uint64_t used_bytes;         /* 已用容量（0 = 未知） */
    int      valid;
};

/* 初始化 VFS 并挂载 C:（tmpfs 系统盘） */
int vfs_init(void);

/* 挂载一个盘（letter 'C'-'Z'；root 为该盘根 inode） */
int vfs_mount_drive(char letter, const char *label, struct inode *root,
                    uint64_t total, uint64_t free);

/* 设置/获取当前盘符（路径无 "X:" 前缀时使用） */
int vfs_set_current_drive(char letter);
char vfs_current_drive(void);

/* 枚举盘：i 从 0 起；返回 1 枚举完 */
int vfs_drive_info(uint32_t i, struct drive *out);
int vfs_drive_count(void);

/* 创建文件/目录（path 的父目录必须存在）；type = VFS_TYPE_* */
int vfs_create(const char *path, uint32_t type, struct inode **out);

/* 打开（按路径查找） */
int vfs_open(const char *path, struct file **out);

/* 读：从 f->pos 起读 len，实际字节数存 *got；f->pos 前进 */
int vfs_read(struct file *f, void *buf, size_t len, size_t *got);

/* 写：从 f->pos 起写 len（必要时扩展文件）；f->pos 前进 */
int vfs_write(struct file *f, const void *buf, size_t len);

/* 关闭（释放 file；inode 常驻） */
int vfs_close(struct file *f);

/* 删除文件/空目录 */
int vfs_unlink(const char *path);

/* 截断 */
int vfs_truncate(const char *path, uint64_t size);

/* 列出目录内容到 buf（"name type size\n" 每项一行） */
int vfs_list(const char *path, char *buf, size_t buflen);

/* 阶段二十：枚举目录第 idx 项（0 起），越界返回 1（名称/类型/大小） */
int vfs_readdir(const char *path, uint64_t idx, char *name,
                uint32_t *type, uint64_t *size);

/* 阶段二十：获取路径元数据（类型/大小/inode 号） */
int vfs_stat(const char *path, uint32_t *type, uint64_t *size,
             uint64_t *ino);

/* 根 inode */
struct inode *vfs_root(void);

#endif /* NOVA_VFS_H */
