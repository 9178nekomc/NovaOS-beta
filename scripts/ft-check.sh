#!/usr/bin/env bash
cd "$(dirname "$0")/.."
echo "=== ftsystem_kernel.c ==="
cat src/graphics/font/ftsystem_kernel.c 2>/dev/null | head -80
echo "=== wenyuan 渲染调用 ==="
grep -n 'FT_Load_Glyph\|FT_LOAD\|FT_Set_Pixel_Sizes\|FT_New_Memory_Face' src/graphics/font/wenyuan.c
