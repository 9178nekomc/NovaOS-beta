/* kernel/font/ftsystem_kernel.c - Nova OS：FreeType 系统接口（内存分配适配）
 *
 * 替代原版 ftsystem.c：
 *   - 原版依赖 stdio（fopen/fclose/fseek/ftell）且 ftstdlib.h 会把
 *     ft_smalloc 宏映射回 malloc（覆盖命令行 -D），无法用于内核；
 *   - 本文件把 FreeType 内存管理接到内核 kmalloc/kfree/krealloc；
 *   - 文件流支持由 Makefile 的 -DFT_CONFIG_OPTION_DISABLE_STREAM_SUPPORT
 *     整体关闭（Nova 只用 FT_New_Memory_Face，无需文件流）。
 */
#include <stddef.h>

#include <ft2build.h>
#include FT_CONFIG_CONFIG_H
#include <freetype/ftsystem.h>
#include <freetype/fttypes.h>
#include <freetype/fterrors.h>
#include <freetype/internal/ftdebug.h>
#include <freetype/internal/ftstream.h>
#include <freetype/internal/ftobjs.h> /* FT_New_Memory / FT_Done_Memory 声明 */

#include "../../core/mm/kmalloc.h"

/* ------------------------------------------------------------------ */
/* 内存分配回调                                                        */
/* ------------------------------------------------------------------ */

FT_CALLBACK_DEF( void* )
ft_alloc( FT_Memory  memory,
          long       size )
{
    (void)memory;
    return kmalloc( (size_t)size );
}

FT_CALLBACK_DEF( void* )
ft_realloc( FT_Memory  memory,
            long       cur_size,
            long       new_size,
            void*      block )
{
    (void)memory;
    (void)cur_size;
    return krealloc( block, (size_t)new_size );
}

FT_CALLBACK_DEF( void )
ft_free( FT_Memory  memory,
         void*      block )
{
    (void)memory;
    kfree( block );
}

/* ------------------------------------------------------------------ */
/* 内存管理器创建 / 销毁（声明见 internal/ftobjs.h）                    */
/* ------------------------------------------------------------------ */

FT_BASE_DEF( FT_Memory )
FT_New_Memory( void )
{
    FT_Memory  memory;

    memory = (FT_Memory)kmalloc( sizeof ( *memory ) );
    if ( memory )
    {
        memory->user    = NULL;
        memory->alloc   = ft_alloc;
        memory->realloc = ft_realloc;
        memory->free    = ft_free;
    }

    return memory;
}

FT_BASE_DEF( void )
FT_Done_Memory( FT_Memory  memory )
{
    kfree( memory );
}

/* END */
