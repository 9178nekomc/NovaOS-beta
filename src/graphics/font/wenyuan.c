/* kernel/font/wenyuan.c - Nova OS 阶段二十：文渊黑体（FreeType）渲染
 *
 * 用 FreeType 渲染内嵌的 WenYuan Sans SC（文渊黑体）TTF：
 *   - 拉丁字符 8x16，CJK 16x16（8-bit 灰度，抗锯齿）
 *   - 内存 face（字体二进制经链接器嵌入 .rodata.font）
 *
 * FreeType 裁剪集成：
 *   - ftsystem_kernel.c 把内存分配接到 kmalloc
 *   - ftdebug_kernel.c 提供空调试输出
 *   - ftmodule.h 只注册 truetype/cff/sfnt/psaux/pshinter/psnames/autofit/
 *     smooth/raster
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_BITMAP_H

#include "../../lib/uart.h"

/* 字形渲染缓冲尺寸：
 * - WENYUAN_PX：字号 16px（FT_Set_Pixel_Sizes 的高度；CJK 满格 16x16）
 * - WENYUAN_W：缓冲宽 16（CJK 满格；Latin 比例字形 3-14px 也容纳）
 * - WENYUAN_H：缓冲高 24 —— 16px 行高 + 下行部（descender）4px 空间。
 *   文渊黑体 16px 字号下 p/q/g/j/y 等字形高 13-17px，基线以下延伸
 *   最多 4px；若缓冲只有 16px 高，descender 被裁剪（"下半部分截断"）。
 * - WENYUAN_BASE：缓冲内基线 y 坐标 = 16（CJK 顶对齐时基线在行底，
 *   Latin 按基线对齐后 descender 落在 16..20 区域）
 * 注意：字号（WENYUAN_PX）与缓冲高（WENYUAN_H）必须分开——
 * 若 FT_Set_Pixel_Sizes 误用缓冲高 24，字形被放大 1.5 倍，
 * CJK 顶部裁掉（像甲骨文）、英文巨大（又扁又大）。 */
#define WENYUAN_PX   16
#define WENYUAN_H    24
#define WENYUAN_W    16
#define WENYUAN_BASE 16

/* 链接器嵌入的字体二进制（objcopy -I binary，符号含源路径前缀） */
extern const uint8_t _binary_src_graphics_font_WenYuanSansSCVF_ttf_start[];
extern const uint8_t _binary_src_graphics_font_WenYuanSansSCVF_ttf_end[];

static FT_Library ft_lib;
static FT_Face ft_face;
static bool wenyuan_ok;

/* 字形位图缓存：避免每个字符都走 FT_Load_Glyph+FT_Render_Glyph
 * （QEMU TCG 下 FreeType 渲染较慢，缓存可提速一个数量级）。
 * 2 路组相联：slot = cp & (WAYS-1)，同组 2 个 entry 线性查 cp。
 * 直接映射（2048 槽）会让低 11 位相同的不同汉字互相覆盖，造成
 * 显示错字（乱码），故改为 2 路。1024 组 x 2 路。 */
#define GLYPH_CACHE_WAYS 2
#define GLYPH_CACHE_SETS 1024
#define GLYPH_CACHE_MASK (GLYPH_CACHE_SETS - 1)

struct glyph_cache_entry {
    uint32_t cp;
    uint8_t  valid;
    uint8_t  width;              /* 字形位图像素宽（CJK~16，Latin 3-14） */
    uint8_t  advance;            /* 排布前进宽度 px（CJK=16，Latin 实际） */
    uint8_t  pixels[WENYUAN_H][WENYUAN_W];  /* 8-bit 灰度 */
};

static struct glyph_cache_entry glyph_cache[GLYPH_CACHE_SETS][GLYPH_CACHE_WAYS];

/* 是否宽字符（CJK 及全角，占 2 列 16px；与 terminal.c 的 is_wide_cp 一致） */
static bool wenyuan_wide_cp(uint32_t cp)
{
    if (cp >= 0x2E80u && cp <= 0x9FFFu)
        return true;   /* CJK 部首/统一表意/平假名/片假名/谚文 */
    if (cp >= 0xF900u && cp <= 0xFAFFu)
        return true;   /* CJK 兼容表意 */
    if (cp >= 0x3000u && cp <= 0x303Fu)
        return true;   /* CJK 标点（全角空格等） */
    if (cp >= 0xFF00u && cp <= 0xFFEFu)
        return true;   /* 全角 ASCII 形式 */
    if (cp >= 0x20000u && cp <= 0x2FFFFu)
        return true;   /* CJK 扩展 B 及以后 */
    return false;
}

/* 渲染码点到 16x16 灰度（含缓存读写）。返回 0 成功；-ENOENT 缺字形。
 * @out_w    返回字形位图像素宽
 * @out_adv  返回排布前进宽度（CJK=16，Latin=实际 advance）
 * 注：所有字符统一 16px 高度渲染，不做水平压缩——Latin 保持原始
 * 比例（'W' 宽 'l' 窄），由终端按 advance 流式排布。 */
static int wenyuan_render_cached(uint32_t cp, uint8_t out[WENYUAN_H][WENYUAN_W],
                                 uint32_t *out_w, uint32_t *out_adv)
{
    uint32_t set = cp & GLYPH_CACHE_MASK;
    struct glyph_cache_entry *ways = glyph_cache[set];
    struct glyph_cache_entry *victim = &ways[0];

    for (int w = 0; w < GLYPH_CACHE_WAYS; w++) {
        if (ways[w].valid && ways[w].cp == cp) {
            *out_w = ways[w].width;
            if (out_adv)
                *out_adv = ways[w].advance;
            memcpy(out, ways[w].pixels, sizeof(ways[w].pixels));
            return 0;
        }
        if (!ways[w].valid)
            victim = &ways[w];
    }

    /* 统一 16px 高度（CJK 16px 宽；Latin 比例，宽 3-14px）。
     * 像素尺寸在 font_sans_init 设置一次，不再每次切换
     * （频繁 FT_Set_Pixel_Sizes 会破坏 face 内部状态）。 */
    bool wide = wenyuan_wide_cp(cp);
    (void)wide;
    FT_UInt gindex = FT_Get_Char_Index(ft_face, (FT_ULong)cp);    if (gindex == 0) {
        /* 调试：首次缺字形时打印码点与 cmap 信息 */
        static bool missing_logged;
        if (!missing_logged) {
            missing_logged = true;
            uart_printf("[Nova] wenyuan: no glyph for U+%04X, "
                        "charmap=%p enc=%d charmaps=%d\r\n",
                        (unsigned)cp,
                        (void *)(uintptr_t)ft_face->charmap,
                        (int)(ft_face->charmap ? ft_face->charmap->encoding
                                               : -1),
                        (int)ft_face->num_charmaps);
        }
        return -ENOENT;
    }

    FT_Error err = FT_Load_Glyph(ft_face, gindex,
                                 FT_LOAD_RENDER | FT_LOAD_NO_HINTING);
    if (err != 0)
        return -EIO;

    FT_GlyphSlot slot2 = ft_face->glyph;
    FT_Bitmap *bmp = &slot2->bitmap;
    int bw = (int)bmp->width;
    int bh = (int)bmp->rows;
    if (bw > WENYUAN_W)
        bw = WENYUAN_W;
    if (bh > WENYUAN_H)
        bh = WENYUAN_H;

    memset(victim->pixels, 0, sizeof(victim->pixels));

    /* 字形定位（基线对齐）：
     * - CJK：bitmap_top≈16（顶对齐），oy = WENYUAN_BASE - 16 = 0，
     *   字形占缓冲顶部 16px
     * - Latin：按基线对齐，oy = WENYUAN_BASE - bitmap_top；
     *   p/q/g/j/y 的 descender 落在 16..20 区域（缓冲高 24，不裁剪）
     * 统一使用 bitmap_left（CJK 通常为 0，Latin 保留原始位置） */
    int ox = (int)slot2->bitmap_left;
    int oy = WENYUAN_BASE - (int)slot2->bitmap_top;

    for (int y = 0; y < bh; y++) {
        int ty = oy + y;
        if (ty < 0 || ty >= WENYUAN_H)
            continue;
        for (int x = 0; x < bw; x++) {
            int tx = ox + x;
            if (tx < 0 || tx >= WENYUAN_W)
                continue;
            uint8_t v;
            if (bmp->pixel_mode == FT_PIXEL_MODE_GRAY) {
                v = bmp->buffer[(uint32_t)y * bmp->pitch + (uint32_t)x];
            } else if (bmp->pixel_mode == FT_PIXEL_MODE_MONO) {
                uint8_t byte =
                    bmp->buffer[(uint32_t)y * bmp->pitch + (uint32_t)(x / 8)];
                v = (byte & (0x80u >> (x % 8))) ? 255 : 0;
            } else {
                v = 0;
            }
            victim->pixels[ty][tx] = v;
        }
    }

    /* 记录字形位图宽 + 排布前进宽度：
     * - CJK：16px（覆盖 2 列）
     * - Latin：实际 advance（'W'=14 'l'=5 等，比例排布） */
    victim->width = wide ? (uint8_t)WENYUAN_W : (uint8_t)bw;
    {
        int adv = (int)(slot2->advance.x >> 6);
        if (adv < 1)
            adv = 1;
        if (adv > 255)
            adv = 255;
        victim->advance = wide ? (uint8_t)WENYUAN_W : (uint8_t)adv;
    }
    victim->cp = cp;
    victim->valid = 1;
    *out_w = victim->width;
    if (out_adv)
        *out_adv = victim->advance;
    memcpy(out, victim->pixels, sizeof(victim->pixels));
    return 0;
}

int font_sans_init(void)
{
    FT_Error err;

    err = FT_Init_FreeType(&ft_lib);
    if (err != 0) {
        uart_printf("[Nova] wenyuan: FT_Init_FreeType failed err=%d\r\n", err);
        return -1;
    }

    long fsize = (long)(_binary_src_graphics_font_WenYuanSansSCVF_ttf_end -
                        _binary_src_graphics_font_WenYuanSansSCVF_ttf_start);
    err = FT_New_Memory_Face(ft_lib, _binary_src_graphics_font_WenYuanSansSCVF_ttf_start,
                             fsize, 0, &ft_face);
    if (err != 0) {
        uart_printf("[Nova] wenyuan: FT_New_Memory_Face failed err=%d\r\n",
                    err);
        return -2;
    }

    err = FT_Set_Pixel_Sizes(ft_face, 0, WENYUAN_PX);
    if (err != 0) {
        uart_printf("[Nova] wenyuan: FT_Set_Pixel_Sizes failed err=%d\r\n",
                    err);
        return -3;
    }

    wenyuan_ok = true;
    uart_printf("[Nova] wenyuan: face '%s' %ld glyphs, %u x %u units\r\n",
                ft_face->family_name ? ft_face->family_name : "?",
                ft_face->num_glyphs,
                (unsigned)ft_face->height,
                (unsigned)ft_face->units_per_EM);

    /* 预热常用字形缓存：ASCII 可打印 + 常用汉字（你好 Nova 文渊黑体
     * 渲染中文成功字体切换命令等）。避免首次输出时逐字符走
     * FT_Load_Glyph+FT_Render_Glyph（QEMU TCG 下每次 ~毫秒级，
     * 全屏重绘 2560 格会卡顿数十秒）。 */
    {
        static const char *warm_text =
            "abcdefghijklmnopqrstuvwxyz"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "0123456789"
            " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
            "你好Nova文渊黑体渲染中文成功字体切换命令"
            "列表当前可用fontsetmonowenyuan加载初始化系统"
            "已切换至显示正常"
            "盘符卷标总容量已用剩余"
            "，。！：；、（）【】《》";
        uint8_t tmp[WENYUAN_W * WENYUAN_H];
        uint32_t tw, ta;
        for (const char *p = warm_text; *p; ) {
            unsigned cp;
            unsigned char b = (unsigned char)*p;
            if (b < 0x80) { cp = b; p++; }
            else if ((b & 0xE0) == 0xC0) {
                cp = ((unsigned)(b & 0x1F) << 6) |
                     ((unsigned char)p[1] & 0x3F); p += 2;
            } else if ((b & 0xF0) == 0xE0) {
                cp = ((unsigned)(b & 0x0F) << 12) |
                     (((unsigned char)p[1] & 0x3F) << 6) |
                     ((unsigned char)p[2] & 0x3F); p += 3;
            } else { cp = b; p++; }
            wenyuan_render_cached(cp, (uint8_t(*)[WENYUAN_W])tmp, &tw, &ta);
        }
        uart_printf("[Nova] wenyuan: glyph cache warmed\r\n");
    }
    return 0;
}

bool font_sans_ready(void)
{
    return wenyuan_ok;
}

/*
 * 渲染码点到 8-bit 灰度缓冲。
 * @out        输出缓冲（FONT 高度行，每行 out_stride 字节）
 * @out_stride 输出每行字节数
 * @out_w      返回字形像素宽度（拉丁 ~8，CJK ~16）
 * 返回 0 成功；负 errno 失败（缺字形 -ENOENT）。
 */
int font_sans_render(uint32_t cp, uint8_t *out, uint32_t out_stride,
                     uint32_t *out_w, uint32_t *out_adv)
{
    uint8_t buf[WENYUAN_H][WENYUAN_W];
    int r;

    if (!wenyuan_ok)
        return -ENODEV;

    r = wenyuan_render_cached(cp, buf, out_w, out_adv);
    if (r != 0)
        return r;

    /* 拷贝到调用方缓冲（每行 out_stride 字节，前 WENYUAN_W 字节有效） */
    for (int y = 0; y < WENYUAN_H; y++) {
        memcpy(out + (uint32_t)y * out_stride, buf[y], WENYUAN_W);
    }
    return 0;
}
