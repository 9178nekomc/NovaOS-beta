/* kernel/fs/vfs.h - Nova OS 阶段十三：tmpfs 后端接口 */
#ifndef NOVA_TMPFS_H
#define NOVA_TMPFS_H

#include "vfs.h"

/* 挂载根 tmpfs：初始化根目录 inode，返回其指针 */
struct inode *tmpfs_mount(void);

#endif /* NOVA_TMPFS_H */
