/* kernel/fs/vfs.c - Nova OS 阶段十三：VFS 路径解析与顶层 API
 *
 * 当前仅根 tmpfs 挂载（"/"）。路径解析：按 '/' 切分组件，
 * 从根 inode 逐级 lookup，最后一段交给 create/unlink。
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "vfs.h"
#include "tmpfs.h"
#include "../core/mm/kmalloc.h"

static struct inode *root;

/* 盘表：C: = tmpfs 系统盘，D:/E:/... = 物理盘 ext2 */
static struct drive drives[VFS_MAX_DRIVES];
static int drive_count;
static char cur_drive = 'C';

static int resolve_dir(const char *path, struct inode **dir_out); /* 前向 */

struct inode *vfs_root(void)
{
    return root;
}

static struct drive *drive_by_letter(char letter)
{
    for (int i = 0; i < drive_count; i++)
        if (drives[i].valid && drives[i].letter == letter)
            return &drives[i];
    return NULL;
}

int vfs_mount_drive(char letter, const char *label, struct inode *droot,
                    uint64_t total, uint64_t free)
{
    if (drive_count >= VFS_MAX_DRIVES)
        return -ENOSPC;
    struct drive *d = &drives[drive_count];
    d->letter = letter;
    d->root = droot;
    d->total_bytes = total;
    d->free_bytes = free;
    d->used_bytes = (total > free) ? (total - free) : 0;
    d->valid = 1;
    strncpy(d->label, label, sizeof(d->label) - 1);
    d->label[sizeof(d->label) - 1] = '\0';
    drive_count++;
    return 0;
}

int vfs_set_current_drive(char letter)
{
    if (drive_by_letter(letter) == NULL)
        return -ENODEV;
    cur_drive = letter;
    return 0;
}

char vfs_current_drive(void)
{
    return cur_drive;
}

int vfs_drive_info(uint32_t i, struct drive *out)
{
    if (i >= (uint32_t)drive_count)
        return 1;
    *out = drives[i];
    return 0;
}

int vfs_drive_count(void)
{
    return drive_count;
}

/* 解析路径：开头的 "X:" 表示盘符；返回该盘根 inode（缺省当前盘）。
 * @path_in  输入路径（可能以 "X:" 开头）
 * @path_out 去除盘符后的路径（"C:/docs" -> "/docs"）
 */
static struct inode *resolve_drive(const char *path_in, char *path_out,
                                   size_t outlen)
{
    char letter = cur_drive;
    if (path_in[0] >= 'A' && path_in[0] <= 'Z' && path_in[1] == ':') {
        letter = path_in[0];
        path_in += 2;
    } else if (path_in[0] >= 'a' && path_in[0] <= 'z' && path_in[1] == ':') {
        letter = path_in[0] - 'a' + 'A';
        path_in += 2;
    }
    struct drive *d = drive_by_letter(letter);
    if (d == NULL)
        return NULL;
    strncpy(path_out, path_in, outlen - 1);
    path_out[outlen - 1] = '\0';
    /* 保证以 '/' 开头（"docs" -> "/docs"） */
    if (path_out[0] != '/') {
        char tmp[VFS_PATH_MAX];
        strncpy(tmp, path_out, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        size_t l = strlen(tmp);
        if (l + 2 < outlen) {
            path_out[0] = '/';
            strcpy(path_out + 1, tmp);
        }
    }
    return d->root;
}

int vfs_init(void)
{
    root = tmpfs_mount();
    if (root == NULL)
        return -ENOMEM;
    /* C: = tmpfs 系统盘 */
    drive_count = 0;
    cur_drive = 'C';
    vfs_mount_drive('C', "sys", root, 0, 0);
    return 0;
}

/* 把路径切分为组件；返回第一个组件或 NULL */
static const char *next_component(const char *path, char *out, size_t outlen)
{
    while (*path == '/')
        path++;
    if (*path == '\0')
        return NULL;
    size_t n = 0;
    while (path[n] != '\0' && path[n] != '/') {
        if (n + 1 >= outlen)
            return NULL;                    /* 组件过长 */
        out[n] = path[n];
        n++;
    }
    out[n] = '\0';
    return path + n;
}

/* 解析 path 的父目录与叶名；失败返回负 errno。
 * 支持 "X:" 盘符前缀；无前缀用当前盘。 */
static int resolve_parent(const char *path, struct inode **dir_out,
                          char *leaf, size_t leaf_len)
{
    if (path == NULL || path[0] == '\0')
        return -EINVAL;

    char p[VFS_PATH_MAX];
    struct inode *droot = resolve_drive(path, p, sizeof(p));
    if (droot == NULL)
        return -ENODEV;
    if (p[0] != '/')
        return -EINVAL;

    struct inode *dir = droot;
    char comp[VFS_MAX_NAME];
    const char *rest = p;

    for (;;) {
        const char *after = next_component(rest, comp, sizeof(comp));
        if (after == NULL) {                /* path 是 "/" */
            return -EINVAL;
        }
        if (*after == '\0') {               /* comp 是最后一段（叶名） */
            if (strlen(comp) + 1 > leaf_len)
                return -EINVAL;
            strcpy(leaf, comp);
            *dir_out = dir;
            return 0;
        }
        /* 中间组件：进入子目录 */
        struct inode *sub = NULL;
        int err = dir->ops->lookup(dir, comp, &sub);
        if (err != 0)
            return err;
        if (sub->type != VFS_TYPE_DIR)
            return -ENOTDIR;
        dir = sub;
        rest = after;
    }
}

int vfs_create(const char *path, uint32_t type, struct inode **out)
{
    struct inode *dir;
    char leaf[VFS_MAX_NAME];
    int err = resolve_parent(path, &dir, leaf, sizeof(leaf));
    if (err != 0)
        return err;
    return dir->ops->create(dir, leaf, type, out);
}

int vfs_open(const char *path, struct file **out)
{
    struct inode *dir;
    char leaf[VFS_MAX_NAME];
    int err = resolve_parent(path, &dir, leaf, sizeof(leaf));
    if (err != 0)
        return err;

    struct inode *in = NULL;
    err = dir->ops->lookup(dir, leaf, &in);
    if (err != 0)
        return err;
    if (in->type != VFS_TYPE_FILE)
        return -EISDIR;

    struct file *f = kcalloc(1, sizeof(struct file));
    if (f == NULL)
        return -ENOMEM;
    f->inode = in;
    f->pos = 0;
    *out = f;
    return 0;
}

int vfs_read(struct file *f, void *buf, size_t len, size_t *got)
{
    if (f == NULL || f->inode == NULL)
        return -EINVAL;
    ssize_t n = f->inode->ops->read(f->inode, buf, len, f->pos);
    if (n < 0)
        return (int)n;
    f->pos += (uint64_t)n;
    if (got)
        *got = (size_t)n;
    return 0;
}

int vfs_write(struct file *f, const void *buf, size_t len)
{
    if (f == NULL || f->inode == NULL)
        return -EINVAL;
    ssize_t n = f->inode->ops->write(f->inode, buf, len, f->pos);
    if (n < 0)
        return (int)n;
    f->pos += (uint64_t)n;
    return 0;
}

int vfs_close(struct file *f)
{
    if (f == NULL)
        return -EINVAL;
    kfree(f);
    return 0;
}

int vfs_unlink(const char *path)
{
    struct inode *dir;
    char leaf[VFS_MAX_NAME];
    int err = resolve_parent(path, &dir, leaf, sizeof(leaf));
    if (err != 0)
        return err;
    return dir->ops->unlink(dir, leaf);
}

int vfs_truncate(const char *path, uint64_t size)
{
    struct inode *dir;
    char leaf[VFS_MAX_NAME];
    int err = resolve_parent(path, &dir, leaf, sizeof(leaf));
    if (err != 0)
        return err;
    struct inode *in = NULL;
    err = dir->ops->lookup(dir, leaf, &in);
    if (err != 0)
        return err;
    return in->ops->truncate(in, size);
}

int vfs_list(const char *path, char *buf, size_t buflen)
{
    struct inode *dir;
    int err0 = resolve_dir(path, &dir);
    if (err0 != 0)
        return err0;

    size_t used = 0;
    for (uint64_t i = 0;; i++) {
        char name[VFS_MAX_NAME];
        uint32_t type;
        int r = dir->ops->readdir(dir, i, name, &type);
        if (r == 1)
            break;                          /* 枚举完毕 */
        if (r != 0)
            return r;
        /* 手动构建 "name type\n"（无 snprintf） */
        const char *tstr = (type == VFS_TYPE_DIR) ? "dir" : "file";
        size_t need = strlen(name) + 1 + strlen(tstr) + 1;
        if (used + need + 1 > buflen)
            return -ENOSPC;                 /* 缓冲区不足 */
        for (const char *p = name; *p; p++)
            buf[used++] = *p;
        buf[used++] = ' ';
        for (const char *p = tstr; *p; p++)
            buf[used++] = *p;
        buf[used++] = '\n';
    }
    if (used == 0 && buflen > 0)
        buf[0] = '\0';
    else if (used < buflen)
        buf[used] = '\0';
    return 0;
}

/* 解析 path 得到目录 inode（path 为盘根返回盘根 inode） */
static int resolve_dir(const char *path, struct inode **dir_out)
{
    char p[VFS_PATH_MAX];
    struct inode *droot = resolve_drive(path, p, sizeof(p));
    if (droot == NULL)
        return -ENODEV;
    struct inode *dir = droot;
    if (strcmp(p, "/") != 0) {
        struct inode *d2;
        char leaf[VFS_MAX_NAME];
        int err = resolve_parent(path, &d2, leaf, sizeof(leaf));
        if (err != 0)
            return err;
        err = d2->ops->lookup(d2, leaf, &dir);
        if (err != 0)
            return err;
        if (dir->type != VFS_TYPE_DIR)
            return -ENOTDIR;
    }
    *dir_out = dir;
    return 0;
}

int vfs_readdir(const char *path, uint64_t idx, char *name,
                uint32_t *type, uint64_t *size)
{
    struct inode *dir;
    int err = resolve_dir(path, &dir);
    if (err != 0)
        return err;
    int r = dir->ops->readdir(dir, idx, name, type);
    if (r != 0)
        return r;
    if (size != NULL) {
        /* 取该项大小（目录取 0） */
        *size = 0;
        if (*type == VFS_TYPE_FILE) {
            struct inode *sub = NULL;
            if (dir->ops->lookup(dir, name, &sub) == 0 && sub != NULL)
                *size = sub->size;
        }
    }
    return 0;
}

int vfs_stat(const char *path, uint32_t *type, uint64_t *size,
             uint64_t *ino)
{
    char p[VFS_PATH_MAX];
    struct inode *droot = resolve_drive(path, p, sizeof(p));
    if (droot == NULL)
        return -ENODEV;
    /* 盘根（"X:" / "X:/" / 当前盘 "/"） */
    if (strcmp(p, "/") == 0) {
        if (type)
            *type = VFS_TYPE_DIR;
        if (size)
            *size = 0;
        if (ino)
            *ino = droot->ino;
        return 0;
    }
    struct inode *dir = droot;
    char leaf[VFS_MAX_NAME];
    int err = resolve_parent(path, &dir, leaf, sizeof(leaf));
    if (err != 0)
        return err;
    struct inode *in = NULL;
    err = dir->ops->lookup(dir, leaf, &in);
    if (err != 0)
        return err;
    if (type)
        *type = in->type;
    if (size)
        *size = in->size;
    if (ino)
        *ino = in->ino;
    return 0;
}
