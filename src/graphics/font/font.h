/* kernel/font/font.h - Nova OS 阶段二：PSF2 字体模块接口
 *
 * 功能：
 *   font_init()      - 校验内嵌 PSF2 字体头（errno 风格返回值）
 *   font_draw_char() - 在帧缓冲 (x,y) 处绘制 8x16 字符位图
 *
 * 字形数据由 scripts/gen-font.py 从 ter-u16n.psf 生成，
 * 见 psf2font.h（字形位图、MSB 优先、逐行）。
 */
#ifndef NOVA_FONT_H
#define NOVA_FONT_H

#include <stdbool.h>
#include <stdint.h>

/* 字体尺寸常量（由生成的 psf2font.h 提供） */
#include "psf2font.h"

#define FONT_WIDTH   PSF2FONT_WIDTH    /* 8 */
#define FONT_HEIGHT  PSF2FONT_HEIGHT   /* 16 */

/*
 * 校验内嵌字体数据。
 * 返回 0 成功；失败返回负 errno（-EINVAL：魔数/参数不一致）。
 */
int font_init(void);

/*
 * 绘制单个字符。
 * @fb     帧缓冲基址（32bpp）
 * @pitch  每行像素字节数
 * @x, @y  字符左上角像素坐标
 * @ch     字符编码（< PSF2FONT_NUMGLYPH）
 * @fg     前景像素值
 * @bg     背景像素值
 */
void font_draw_char(uint32_t *fb, uint64_t pitch, int x, int y,
                    uint32_t ch, uint32_t fg, uint32_t bg);

/* ------------------------------------------------------------------ */
/* CJK 位图字体（HZK16，构建期渲染，取代 FreeType 文渊黑体）          */
/* ------------------------------------------------------------------ */

/*
 * 初始化 CJK 位图字体（解析内嵌 HZKH blob；font_cjk.c）。
 * 返回 0 成功；负 errno 失败（-EINVAL 魔数/-EIO blob 缺失）。
 */
int font_sans_init(void);

/* CJK 位图字体是否已就绪 */
bool font_sans_ready(void);

/*
 * 渲染码点到 8-bit 灰度缓冲（位图查表：255/0，无抗锯齿）。
 * @out        输出缓冲（16 行，每行 out_stride 字节，前 16 字节有效）
 * @out_stride 输出每行字节数
 * @out_w      返回字形像素宽（16）
 * @out_adv    返回排布前进宽度（16，可 NULL）
 * 返回 0 成功；负 errno 失败（缺字形 -ENOENT）。
 */
int font_sans_render(uint32_t cp, uint8_t *out, uint32_t out_stride,
                     uint32_t *out_w, uint32_t *out_adv);

#endif /* NOVA_FONT_H */
