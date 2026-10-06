/* kernel/font/font_cjk.c - Nova v2：CJK 16x16 位图字体（HBOS 架构模式）
 *
 * 根治坑 4.1（FreeType CJK 渲染越界写）：CJK 字形在构建期由
 * scripts/gen-cjk.py 从文渊 TTF 渲染成 HZK16 位图 blob（magic "HZKH"），
 * 经 objcopy 嵌入 .rodata.cjk。运行时只做二分查找 + 逐位 blit，
 * 没有任何可变长字形解析路径，越界写成为不可能。
 *
 * 对外保持 font_sans_* 接口（font.h），terminal.c 无需改动：
 *   font_sans_init()   - 校验/定位内嵌 blob
 *   font_sans_ready()  - 是否就绪
 *   font_sans_render() - 码点 -> 16x16 灰度（255/0），advance=16
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../../lib/uart.h"

#define CJK_GLYPH_SIZE  16
#define CJK_GLYPH_BYTES 32

/* objcopy -I binary 嵌入（--rename-section .data=.rodata.cjk） */
extern const uint8_t _binary_build_font_cjk_bin_start[];
extern const uint8_t _binary_build_font_cjk_bin_end[];

static const uint8_t  *g_font;      /* blob 基址 */
static uint32_t        g_count;     /* 字形数 */
static const uint32_t *g_codepoints;/* 升序码点表 */
static const uint8_t  *g_bitmaps;   /* count * 32B */
static bool            g_ok;

/* 是否宽字符（与 terminal.c is_wide_cp / 旧 wenyuan_wide_cp 一致） */
static bool cjk_wide_cp(uint32_t cp)
{
    if (cp >= 0x2E80u && cp <= 0x9FFFu)
        return true;
    if (cp >= 0xF900u && cp <= 0xFAFFu)
        return true;
    if (cp >= 0x3000u && cp <= 0x303Fu)
        return true;
    if (cp >= 0xFF00u && cp <= 0xFFEFu)
        return true;
    if (cp >= 0x20000u && cp <= 0x2FFFFu)
        return true;
    return false;
}

int font_sans_init(void)
{
    g_font = _binary_build_font_cjk_bin_start;
    const uint8_t *end = _binary_build_font_cjk_bin_end;
    if (g_font == NULL || (size_t)(end - g_font) < 8) {
        uart_printf("[Nova] font_cjk: empty blob\r\n");
        return -EIO;
    }
    if (g_font[0] != 'H' || g_font[1] != 'Z' ||
        g_font[2] != 'K' || g_font[3] != 'H') {
        uart_printf("[Nova] font_cjk: bad magic\r\n");
        return -EINVAL;
    }
    memcpy(&g_count, g_font + 4, 4);
    g_codepoints = (const uint32_t *)(g_font + 8);
    g_bitmaps = g_font + 8 + (size_t)g_count * 4;
    size_t need = 8 + (size_t)g_count * 4 + (size_t)g_count * CJK_GLYPH_BYTES;
    if ((size_t)(end - g_font) < need) {
        uart_printf("[Nova] font_cjk: blob too small (%u glyphs)\r\n",
                    (unsigned)g_count);
        return -EINVAL;
    }
    g_ok = true;
    uart_printf("[Nova] font_cjk: %u glyphs, %u bytes blob\r\n",
                (unsigned)g_count,
                (unsigned)(end - g_font));
    return 0;
}

bool font_sans_ready(void)
{
    return g_ok;
}

/* 二分查找码点，命中返回 32B 位图指针；未命中返回 NULL */
static const uint8_t *cjk_lookup(uint32_t cp)
{
    if (!g_ok)
        return NULL;
    int lo = 0, hi = (int)g_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint32_t v = g_codepoints[mid];
        if (v == cp)
            return g_bitmaps + (size_t)mid * CJK_GLYPH_BYTES;
        if (v < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/*
 * 渲染码点到 8-bit 灰度缓冲。
 * @out        输出缓冲（16 行，每行 out_stride 字节）
 * @out_stride 输出每行字节数
 * @out_w      返回字形像素宽（16）
 * @out_adv    返回排布前进宽度（16）
 * 返回 0 成功；-ENOENT 缺字形；-ENODEV 未初始化。
 */
int font_sans_render(uint32_t cp, uint8_t *out, uint32_t out_stride,
                     uint32_t *out_w, uint32_t *out_adv)
{
    const uint8_t *bm;
    if (!g_ok)
        return -ENODEV;
    bm = cjk_lookup(cp);
    if (bm == NULL)
        return -ENOENT;

    for (int row = 0; row < CJK_GLYPH_SIZE; row++) {
        uint8_t b0 = bm[row * 2];
        uint8_t b1 = bm[row * 2 + 1];
        uint8_t *dst = out + (uint32_t)row * out_stride;
        for (int col = 0; col < 8; col++) {
            dst[col] = (b0 & (0x80u >> col)) ? 255 : 0;
            dst[8 + col] = (b1 & (0x80u >> col)) ? 255 : 0;
        }
    }
    if (out_w)
        *out_w = CJK_GLYPH_SIZE;
    if (out_adv)
        *out_adv = CJK_GLYPH_SIZE;
    return 0;
}
