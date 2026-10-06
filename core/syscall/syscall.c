/* kernel/syscall/syscall.c - Nova OS 阶段十二：系统调用实现
 *
 * INT 0x80 门（DPL=3）分发：
 *   - SYS_WRITE：fd=1 终端+串口；fd=2 仅串口（用户演示输出）
 *   - SYS_EXIT：任务退出（不返回，任务切换走）
 *   - SYS_GETPID / SYS_GETTICKS / SYS_YIELD
 * 兼容阶段三的 INT 0x80 自检（rax=42 时输出原有串口行）。
 *
 * 注意：write 的缓冲区位于用户地址，本阶段内核与用户共享页表且
 * 未启用 SMAP，内核可直接读取（后续阶段再做地址校验/拷贝）。
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "syscall.h"
#include "../idt/idt.h"
#include "../sched/sched.h"
#include "../../graphics/terminal/terminal.h"
#include "../timer/timer.h"
#include "../../lib/uart.h"

static void sys_write_impl(int fd, const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\n') {
            if (fd == 1)
                terminal_putchar('\n');   /* 终端换行（仅一次） */
            uart_putc('\r');              /* 串口 \r\n */
        } else if (fd == 1) {
            terminal_putchar(c);
        }
        uart_putc(c);
    }
}

static void int80_handler(struct isr_frame *frame)
{
    uint64_t nr = frame->rax;
    uint64_t a0 = frame->rdi;
    uint64_t a1 = frame->rsi;
    uint64_t a2 = frame->rdx;
    long ret = 0;

    switch (nr) {
    case SYS_EXIT:
        task_exit((int)a0);             /* 不返回：任务切走 */
        break;
    case SYS_WRITE:
        if (a0 == 1 || a0 == 2)
            sys_write_impl((int)a0, (const char *)(uintptr_t)a1, (size_t)a2);
        else
            ret = -EINVAL;
        break;
    case SYS_GETPID:
        ret = (long)current_task()->pid;
        break;
    case SYS_GETTICKS:
        ret = (long)timer_get_ticks();
        break;
    case SYS_YIELD:
        yield();
        break;
    case 42:                            /* 阶段三兼容：INT 0x80 自检 */
        uart_printf("[Nova] ISR 0x80 handler: rax=42 rbx=0x%x\n",
                    (unsigned)frame->rbx);
        break;
    default:
        ret = -ENOSYS;
        break;
    }

    frame->rax = (uint64_t)ret;         /* 返回值（SYS_EXIT 不会到达） */
}

int syscall_init(void)
{
    /* DPL=3 中断门：允许 ring3 触发 INT 0x80 */
    return idt_install_user_vector(0x80, int80_handler);
}
