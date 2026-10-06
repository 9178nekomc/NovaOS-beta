/* kernel/syscall/syscall.h - Nova OS 阶段十二：系统调用接口
 *
 * 通过 INT 0x80（DPL=3 中断门）从 ring3 进入：
 *   调用约定：rax = 系统调用号，rdi/rsi/rdx = 参数（SysV），
 *   返回值在 rax。
 */
#ifndef NOVA_SYSCALL_H
#define NOVA_SYSCALL_H

#include <stdint.h>

/* 系统调用号 */
#define SYS_EXIT     0   /* exit(int code) —— 不返回 */
#define SYS_WRITE    1   /* write(int fd, const char *buf, size_t len) */
#define SYS_GETPID   2   /* getpid(void) */
#define SYS_GETTICKS 3   /* getticks(void) —— 本 CPU tick 计数（ms） */
#define SYS_YIELD    4   /* yield(void) */

/*
 * 安装 INT 0x80 系统调用门（DPL=3，允许 ring3 触发）。
 * 返回 0 成功；失败返回负 errno。
 */
int syscall_init(void);

#endif /* NOVA_SYSCALL_H */
