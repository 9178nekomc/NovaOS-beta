/* kernel/fs/ext2fs.c - Nova OS：ext2 卷接入 VFS（多盘支持，可写）
 *
 * 把 ext2 槽位包装成 VFS inode 树，挂到盘表（D:/E:/...）。
 * 读写均支持：create/write/unlink/truncate 落到 ext2 写原语
 * （位图/间接表/inode 立即写回，超级块计数在操作结束时同步）。
 * 每个 VFS inode 的 private 保存 ext2 inode 号；操作前 ext2_select 切槽位。
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "vfs.h"
#include "ext2/ext2.h"
#include "../core/mm/kmalloc.h"

struct ext2_ino {
    int slot;              /* ext2 槽位 */
    uint32_t ino;          /* ext2 inode 号 */
    uint32_t type;         /* VFS_TYPE_* */
    uint64_t size;
};

static int efs_create(struct inode *dir, const char *name, uint32_t type,
                      struct inode **out)
{
    struct ext2_ino *di = (struct ext2_ino *)dir->private;
    if (di == NULL || di->type != VFS_TYPE_DIR)
        return -ENOTDIR;
    if (ext2_select(di->slot) != 0)
        return -EIO;

    uint32_t ino;
    int err = ext2_create(di->ino, name, type, &ino);
    if (err != 0)
        return err;

    struct ext2_inode fi;
    if (ext2_read_inode(ino, &fi) != 0)
        return -EIO;

    struct ext2_ino *ni = kmalloc(sizeof(*ni));
    struct inode *in = kmalloc(sizeof(*in));
    if (ni == NULL || in == NULL) {
        kfree(ni);
        kfree(in);
        return -ENOMEM;
    }
    ni->slot = di->slot;
    ni->ino = ino;
    ni->type = type;
    ni->size = fi.size;
    in->ino = ino;
    in->type = type;
    in->size = fi.size;
    in->ops = dir->ops;
    in->private = ni;
    if (out != NULL)
        *out = in;
    else {               /* 调用方不要结果（如重定向 create）：释放包装 */
        kfree(in);
        kfree(ni);
    }
    return 0;
}

static int efs_lookup(struct inode *dir, const char *name, struct inode **out)
{
    struct ext2_ino *di = (struct ext2_ino *)dir->private;
    if (di == NULL || di->type != VFS_TYPE_DIR)
        return -ENOTDIR;
    if (ext2_select(di->slot) != 0)
        return -EIO;

    struct ext2_inode de;
    if (ext2_read_inode(di->ino, &de) != 0)
        return -EIO;
    uint32_t ino = ext2_lookup(&de, name);
    if (ino == 0)
        return -ENOENT;

    struct ext2_inode fi;
    if (ext2_read_inode(ino, &fi) != 0)
        return -EIO;

    struct ext2_ino *ni = kmalloc(sizeof(*ni));
    if (ni == NULL)
        return -ENOMEM;
    ni->slot = di->slot;
    ni->ino = ino;
    ni->type = (fi.mode & 0xF000u) == 0x4000u ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    ni->size = fi.size;

    struct inode *in = kmalloc(sizeof(*in));
    if (in == NULL) {
        kfree(ni);
        return -ENOMEM;
    }
    in->ino = ino;
    in->type = ni->type;
    in->size = fi.size;
    in->ops = dir->ops;
    in->private = ni;
    *out = in;
    return 0;
}

static int efs_unlink(struct inode *dir, const char *name)
{
    struct ext2_ino *di = (struct ext2_ino *)dir->private;
    if (di == NULL || di->type != VFS_TYPE_DIR)
        return -ENOTDIR;
    if (ext2_select(di->slot) != 0)
        return -EIO;
    return ext2_unlink(di->ino, name);
}

static ssize_t efs_read(struct inode *in, void *buf, size_t len, uint64_t off)
{
    struct ext2_ino *di = (struct ext2_ino *)in->private;
    if (di == NULL || di->type != VFS_TYPE_FILE)
        return -EISDIR;
    if (ext2_select(di->slot) != 0)
        return -EIO;
    struct ext2_inode fi;
    if (ext2_read_inode(di->ino, &fi) != 0)
        return -EIO;
    if (off >= fi.size)
        return 0;
    if (off + len > fi.size)
        len = (size_t)(fi.size - off);
    uint64_t got = 0;
    int err = ext2_read_file(&fi, off, buf, len, &got);
    if (err != 0)
        return err;
    return (ssize_t)got;
}

static ssize_t efs_write(struct inode *in, const void *buf, size_t len,
                         uint64_t off)
{
    struct ext2_ino *di = (struct ext2_ino *)in->private;
    if (di == NULL || di->type != VFS_TYPE_FILE)
        return -EISDIR;
    if (ext2_select(di->slot) != 0)
        return -EIO;
    int err = ext2_write_file(di->ino, off, buf, len);
    if (err != 0)
        return err;
    if (off + len > in->size)
        in->size = off + len;
    return (ssize_t)len;
}

static int efs_readdir(struct inode *dir, uint64_t idx, char *name,
                       uint32_t *type)
{
    struct ext2_ino *di = (struct ext2_ino *)dir->private;
    if (di == NULL || di->type != VFS_TYPE_DIR)
        return -ENOTDIR;
    if (ext2_select(di->slot) != 0)
        return -EIO;
    struct ext2_inode de;
    if (ext2_read_inode(di->ino, &de) != 0)
        return -EIO;
    uint32_t t, ino;
    int r = ext2_read_dir(&de, idx, name, &t, &ino);
    if (r == 1)
        return 1;
    if (r != 0)
        return r;
    *type = (t == EXT2_FT_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    return 0;
}

static int efs_truncate(struct inode *in, uint64_t size)
{
    struct ext2_ino *di = (struct ext2_ino *)in->private;
    if (di == NULL)
        return -EINVAL;
    if (ext2_select(di->slot) != 0)
        return -EIO;
    int err = ext2_truncate(di->ino, size);
    if (err == 0)
        in->size = size;
    return err;
}

static struct fs_ops ext2_ops = {
    efs_create, efs_lookup, efs_unlink,
    efs_read, efs_write, efs_readdir, efs_truncate,
};

/* 挂载 ext2 槽位为 VFS 盘根；返回根 inode（NULL 失败） */
struct inode *ext2fs_mount_root(int slot)
{
    struct ext2_fs *fs = ext2_fs_get(slot);
    if (fs == NULL)
        return NULL;
    if (ext2_select(slot) != 0)
        return NULL;

    struct ext2_ino *ri = kmalloc(sizeof(*ri));
    if (ri == NULL)
        return NULL;
    ri->slot = slot;
    ri->ino = EXT2_ROOT_INO;
    ri->type = VFS_TYPE_DIR;
    ri->size = 0;

    struct inode *root = kmalloc(sizeof(*root));
    if (root == NULL) {
        kfree(ri);
        return NULL;
    }
    root->ino = EXT2_ROOT_INO;
    root->type = VFS_TYPE_DIR;
    root->size = 0;
    root->ops = &ext2_ops;
    root->private = ri;
    return root;
}
