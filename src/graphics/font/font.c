/* kernel/font/font.c - Nova OS 阶段二：PSF2 字体模块实现
 *
 * 字体数据（kernel/font/psf2font.h）由 scripts/gen-font.py 生成：
 *   - psf2font_header[32]  ：原始 PSF2 文件头（font_init() 校验）
 *   - psf2font_glyphs[256][16]：字形位图，MSB 优先，逐行 8 位
 *
 * PSF2 头格式（小端）：
 *   magic(u32) version(u32) headersize(u32) flags(u32)
 *   numglyph(u32) bytesperglyph(u32) height(u32) width(u32)
 */
#include <errno.h>
#include <stdint.h>

#include "font.h"
#include "psf2font.h"

#define PSF2_MAGIC 0x864ab572u

/* 与 PSF2 规范一致的打包头结构 */
struct psf2_header {
    uint32_t magic;
    uint32_t version;
    uint32_t headersize;
    uint32_t flags;
    uint32_t numglyph;
    uint32_t bytesperglyph;
    uint32_t height;
    uint32_t width;
} __attribute__((packed));

int font_init(void)
{
    const struct psf2_header *h =
        (const struct psf2_header *)(const void *)psf2font_header;

    if (h->magic != PSF2_MAGIC)
        return -EINVAL;
    if (h->version != 0 || h->headersize != 32)
        return -EINVAL;
    if (h->numglyph != PSF2FONT_NUMGLYPH ||
        h->bytesperglyph != PSF2FONT_BYTESPERGLYPH)
        return -EINVAL;
    if (h->width != PSF2FONT_WIDTH || h->height != PSF2FONT_HEIGHT)
        return -EINVAL;

    return 0;
}

void font_draw_char(uint32_t *fb, uint64_t pitch, int x, int y,
                    uint32_t ch, uint32_t fg, uint32_t bg)
{
    if (ch >= PSF2FONT_NUMGLYPH)
        return;

    const uint8_t *glyph = psf2font_glyphs[ch];
    const uint32_t stride = (uint32_t)(pitch / 4);

    for (int row = 0; row < PSF2FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        uint32_t *line = fb + (y + row) * stride + x;

        for (int col = 0; col < PSF2FONT_WIDTH; col++) {
            /* 每字节的 MSB 对应最左像素 */
            line[col] = (bits & (0x80u >> col)) ? fg : bg;
        }
    }
}
