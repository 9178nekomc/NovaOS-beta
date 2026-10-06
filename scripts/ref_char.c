/* scripts/ref_char.c - 渲染单个字符为 16x16 灰度 PPM（与内核渲染一致）
 * 用法: ./ref_char <font.ttf> <char-utf8> <out.ppm>
 * Latin 用 bitmap_left 偏移，CJK 网格左对齐（ox=0），顶对齐 oy。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H

static unsigned utf8_next(const char **p)
{
    unsigned char b = (unsigned char)**p;
    unsigned cp;
    if (b < 0x80) { cp = b; (*p)++; }
    else if ((b & 0xE0) == 0xC0) { cp = (b & 0x1F) << 6 | ((unsigned char)(*p)[1] & 0x3F); *p += 2; }
    else if ((b & 0xF0) == 0xE0) { cp = (b & 0x0F) << 12 | ((unsigned char)(*p)[1] & 0x3F) << 6 | ((unsigned char)(*p)[2] & 0x3F); *p += 3; }
    else { cp = (unsigned char)**p; (*p)++; }
    return cp;
}

static unsigned char *read_file(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    *size = n;
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s font char out.ppm\n", argv[0]); return 1; }
    FT_Library lib;
    FT_Face face;
    FT_Init_FreeType(&lib);
    /* 与内核一致：FT_New_Memory_Face 内存加载（避免文件流依赖） */
    long fsize = 0;
    unsigned char *fdata = read_file(argv[1], &fsize);
    if (!fdata) { fprintf(stderr, "read fail\n"); return 1; }
    if (FT_New_Memory_Face(lib, fdata, fsize, 0, &face) != 0) {
        fprintf(stderr, "face fail\n"); free(fdata); return 1;
    }
    FT_Set_Pixel_Sizes(face, 0, 16);

    const char *p = argv[2];
    unsigned cp = utf8_next(&p);
    FT_UInt gi = FT_Get_Char_Index(face, cp);
    /* 与内核一致：FT_LOAD_TARGET_LIGHT（轻 hinting，字形细清晰） */
    FT_Error err = FT_Load_Glyph(face, gi,
                                 FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT);
    if (err != 0) { fprintf(stderr, "load fail cp=%04x\n", cp); free(fdata); return 1; }

    unsigned char img[16 * 16];
    memset(img, 0, sizeof(img));
    FT_Bitmap *bm = &face->glyph->bitmap;
    int ox = (cp >= 0x2E80u) ? 0 : (int)face->glyph->bitmap_left;
    int oy = 16 - (int)face->glyph->bitmap_top;
    int bw = bm->width > 16 ? 16 : (int)bm->width;
    int bh = bm->rows > 16 ? 16 : (int)bm->rows;
    for (int y = 0; y < bh; y++) {
        int ty = oy + y;
        if (ty < 0 || ty >= 16) continue;
        for (int x = 0; x < bw; x++) {
            int tx = ox + x;
            if (tx < 0 || tx >= 16) continue;
            img[ty * 16 + tx] = bm->buffer[y * bm->pitch + x];
        }
    }
    FILE *f = fopen(argv[3], "wb");
    fprintf(f, "P6\n16 16\n255\n");
    for (int i = 0; i < 256; i++) {
        fputc(img[i], f); fputc(img[i], f); fputc(img[i], f);
    }
    fclose(f);
    printf("ref char U+%04X saved %s\n", cp, argv[3]);
    return 0;
}
