/* kernel/font/ftdebug_kernel.c - Nova OS：FreeType 调试输出适配
 *
 * FreeType 内部可能引用 FT_Message/FT_Panic/FT_Throw/FT_Trace_*
 * （即使 FT_DEBUG_LEVEL_* 关闭时部分路径仍引用），这里无条件提供，
 * 输出重定向到内核串口调试。
 */
#include <stdarg.h>

#include <ft2build.h>
#include FT_CONFIG_CONFIG_H
#include <freetype/internal/ftdebug.h>
#include <freetype/fttypes.h>

#include "../../lib/uart.h"

/* 打印消息（同 FT_Message） */
void
FT_Message(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    uart_vprintf(fmt, ap);
    va_end(ap);
    uart_puts("\r\n");
}

/* 打印消息并停机（同 FT_Panic） */
void
FT_Panic(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    uart_vprintf(fmt, ap);
    va_end(ap);
    uart_puts("\r\n");
    for (;;)
        __asm__ volatile("hlt");
}

/* 报告错误位置（同 FT_Throw，返回错误码本身） */
int
FT_Throw(FT_Error error, int line, const char *file)
{
    (void)line;
    (void)file;
    return (int)error;
}

void
FT_Trace_Disable(void)
{
}

void
FT_Trace_Enable(void)
{
}
