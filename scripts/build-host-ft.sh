#!/usr/bin/env bash
# scripts/build-host-ft.sh - 用内核同源 FreeType 2.13.3 编译宿主渲染器
set -euo pipefail
cd "$(dirname "$0")/.."

# 用内核的 FreeType 源码编译（聚合最小集），链接宿主渲染器
FT=third_party/freetype
OBJ=build/host-ft
mkdir -p "$OBJ"

SRCS="$FT/src/base/ftbase.c $FT/src/base/ftbitmap.c $FT/src/base/ftinit.c \
      $FT/src/base/ftmm.c \
      $FT/src/truetype/truetype.c $FT/src/cff/cff.c $FT/src/sfnt/sfnt.c \
      $FT/src/psaux/psaux.c $FT/src/pshinter/pshinter.c $FT/src/psnames/psnames.c \
      $FT/src/smooth/smooth.c $FT/src/raster/raster.c $FT/src/autofit/autofit.c"

for s in $SRCS; do
    name=$(basename "$s" .c)
    if [ ! -f "$OBJ/$name.o" ]; then
        gcc -I"$FT/include" -DFT2_BUILD_LIBRARY \
            -DFT_CONFIG_OPTION_DISABLE_STREAM_SUPPORT -O2 -c "$s" -o "$OBJ/$name.o"
    fi
done

# ftsystem/调试：宿主用标准 C 版（避免内核依赖）
cat > "$OBJ/ftsystem_host.c" <<'EOF'
#include <stdlib.h>
#include <ft2build.h>
#include FT_CONFIG_CONFIG_H
#include <freetype/internal/ftobjs.h>
#include <freetype/ftsystem.h>
#include <freetype/fttypes.h>
static void *h_alloc(FT_Memory m, long s) { (void)m; return malloc((size_t)s); }
static void h_free(FT_Memory m, void *b) { (void)m; free(b); }
static void *h_realloc(FT_Memory m, long c, long n, void *b) { (void)m; (void)c; return realloc(b, (size_t)n); }
FT_BASE_DEF(FT_Memory) FT_New_Memory(void) {
    FT_Memory mem = (FT_Memory)malloc(sizeof(*mem));
    if (mem) { mem->user = NULL; mem->alloc = h_alloc; mem->realloc = h_realloc; mem->free = h_free; }
    return mem;
}
FT_BASE_DEF(void) FT_Done_Memory(FT_Memory m) { free(m); }
EOF
cat > "$OBJ/ftdebug_host.c" <<'EOF'
#include <ft2build.h>
#include FT_CONFIG_CONFIG_H
#include <freetype/internal/ftdebug.h>
void FT_Trace_Disable(void) {}
void FT_Trace_Enable(void) {}
void FT_Message(const char *fmt, ...) { (void)fmt; }
void FT_Panic(const char *fmt, ...) { (void)fmt; }
int FT_Throw(FT_Error e, int l, const char *f) { (void)l; (void)f; return (int)e; }
EOF
gcc -I"$FT/include" -DFT2_BUILD_LIBRARY -O2 -c "$OBJ/ftsystem_host.c" -o "$OBJ/ftsystem.o"
gcc -I"$FT/include" -DFT2_BUILD_LIBRARY -O2 -c "$OBJ/ftdebug_host.c" -o "$OBJ/ftdebug.o"

# 链接成静态库
ar rcs "$OBJ/libft2133.a" "$OBJ"/*.o
echo "built $OBJ/libft2133.a"
