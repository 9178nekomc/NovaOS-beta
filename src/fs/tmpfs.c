/* kernel/fs/tmpfs.c - Nova OS 阶段十三：tmpfs 内存文件系统实现
 *
 * 数据全部驻留内存（kmalloc）：
 *   - 文件：data 缓冲区（按需增长）+ capacity
 *   - 目录：单向链表 children（name -> tmpfs_node *）
 * 无持久化；断电即失。单核使用（阶段十三测试运行于 BSP）。
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "tmpfs.h"
#include "../core/mm/kmalloc.h"

#define TMPFS_MIN_CAP 64u

struct tmpfs_entry;

struct tmpfs_node {
    struct inode vfs;
    char *data;                     /* 文件数据（仅文件） */
    uint64_t capacity;
    struct tmpfs_entry *children;   /* 目录条目链表（仅目录） */
};

struct tmpfs_entry {
    char name[VFS_MAX_NAME];
    struct tmpfs_node *node;
    struct tmpfs_entry *next;
};

static uint64_t next_ino = 1;

static struct fs_ops tmpfs_ops;   /* 前置声明见下方赋值 */

static struct tmpfs_node *node_alloc(uint32_t type)
{
    struct tmpfs_node *n = kcalloc(1, sizeof(struct tmpfs_node));
    if (n == NULL)
        return NULL;
    n->vfs.ino = next_ino++;
    n->vfs.type = type;
    n->vfs.size = 0;
    n->vfs.ops = &tmpfs_ops;    /* 所有节点都挂同一操作表 */
    n->vfs.private = n;
    return n;
}

/* ---- 后端操作 ---- */

static int t_create(struct inode *dir_in, const char *name, uint32_t type,
                    struct inode **out)
{
    if (dir_in->type != VFS_TYPE_DIR)
        return -ENOTDIR;
    struct tmpfs_node *dir = dir_in->private;

    for (struct tmpfs_entry *e = dir->children; e; e = e->next) {
        if (strcmp(e->name, name) == 0)
            return -EEXIST;
    }

    struct tmpfs_node *n = node_alloc(type);
    if (n == NULL)
        return -ENOMEM;

    struct tmpfs_entry *e = kcalloc(1, sizeof(struct tmpfs_entry));
    if (e == NULL) {
        kfree(n);
        return -ENOMEM;
    }
    strncpy(e->name, name, VFS_MAX_NAME - 1);
    e->name[VFS_MAX_NAME - 1] = '\0';
    e->node = n;
    e->next = dir->children;
    dir->children = e;

    if (out)
        *out = &n->vfs;
    return 0;
}

static int t_lookup(struct inode *dir_in, const char *name,
                    struct inode **out)
{
    if (dir_in->type != VFS_TYPE_DIR)
        return -ENOTDIR;
    struct tmpfs_node *dir = dir_in->private;

    for (struct tmpfs_entry *e = dir->children; e; e = e->next) {
        if (strcmp(e->name, name) == 0) {
            *out = &e->node->vfs;
            return 0;
        }
    }
    return -ENOENT;
}

static int t_unlink(struct inode *dir_in, const char *name)
{
    if (dir_in->type != VFS_TYPE_DIR)
        return -ENOTDIR;
    struct tmpfs_node *dir = dir_in->private;

    struct tmpfs_entry **pp = &dir->children;
    while (*pp) {
        struct tmpfs_entry *e = *pp;
        if (strcmp(e->name, name) == 0) {
            if (e->node->vfs.type == VFS_TYPE_DIR && e->node->children)
                return -ENOTEMPTY;          /* 非空目录不可删 */
            *pp = e->next;
            kfree(e->node->data);
            kfree(e->node);
            kfree(e);
            return 0;
        }
        pp = &e->next;
    }
    return -ENOENT;
}

static ssize_t t_read(struct inode *in, void *buf, size_t len, uint64_t off)
{
    if (in->type != VFS_TYPE_FILE)
        return -EISDIR;
    if (off >= in->size)
        return 0;
    if (len > in->size - off)
        len = (size_t)(in->size - off);
    struct tmpfs_node *n = in->private;
    memcpy(buf, n->data + off, len);
    return (ssize_t)len;
}

static ssize_t t_write(struct inode *in, const void *buf, size_t len,
                       uint64_t off)
{
    if (in->type != VFS_TYPE_FILE)
        return -EISDIR;
    struct tmpfs_node *n = in->private;
    uint64_t need = off + len;
    if (need > n->capacity) {
        uint64_t new_cap = n->capacity ? n->capacity : TMPFS_MIN_CAP;
        while (new_cap < need)
            new_cap *= 2;
        char *nd = kmalloc((size_t)new_cap);
        if (nd == NULL)
            return -ENOMEM;
        if (n->data) {
            memcpy(nd, n->data, (size_t)in->size);
            kfree(n->data);
        }
        n->data = nd;
        n->capacity = new_cap;
    }
    memcpy(n->data + off, buf, len);
    if (need > in->size)
        in->size = need;
    return (ssize_t)len;
}

static int t_readdir(struct inode *dir_in, uint64_t idx, char *name,
                     uint32_t *type)
{
    if (dir_in->type != VFS_TYPE_DIR)
        return -ENOTDIR;
    struct tmpfs_node *dir = dir_in->private;
    struct tmpfs_entry *e = dir->children;
    for (uint64_t i = 0; e; i++, e = e->next) {
        if (i == idx) {
            strcpy(name, e->name);
            *type = e->node->vfs.type;
            return 0;
        }
    }
    return 1;                               /* 枚举完毕 */
}

static int t_truncate(struct inode *in, uint64_t size)
{
    if (in->type != VFS_TYPE_FILE)
        return -EISDIR;
    struct tmpfs_node *n = in->private;
    if (size > n->capacity) {
        uint64_t new_cap = n->capacity ? n->capacity : TMPFS_MIN_CAP;
        while (new_cap < size)
            new_cap *= 2;
        char *nd = kmalloc((size_t)new_cap);
        if (nd == NULL)
            return -ENOMEM;
        if (n->data) {
            memcpy(nd, n->data, (size_t)in->size);
            kfree(n->data);
        }
        n->data = nd;
        n->capacity = new_cap;
    }
    in->size = size;
    return 0;
}

static struct fs_ops tmpfs_ops = {
    .create   = t_create,
    .lookup   = t_lookup,
    .unlink   = t_unlink,
    .read     = t_read,
    .write    = t_write,
    .readdir  = t_readdir,
    .truncate = t_truncate,
};

struct inode *tmpfs_mount(void)
{
    struct tmpfs_node *root = node_alloc(VFS_TYPE_DIR);
    return root ? &root->vfs : NULL;
}
