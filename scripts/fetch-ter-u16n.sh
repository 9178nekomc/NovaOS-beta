#!/usr/bin/env bash
# scripts/fetch-ter-u16n.sh - 获取 PSF2 格式的 Terminus 8x16 字体
# 依次尝试：Debian 包 / 官方源码包，验证 magic==0x864ab572 (PSF2)
set -euo pipefail

cd "$(dirname "$0")/.."
DEST="src/graphics/font/ter-u16n.psf"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fetch_from_deb() {
    local url="$1" name="$2"
    echo "[Nova] trying $name ..."
    curl -sSL -o "$TMP/pkg.deb" "$url" || return 1
    dpkg-deb -x "$TMP/pkg.deb" "$TMP/x" 2>/dev/null || return 1
    find "$TMP/x" -name 'ter-u16n.psf*' | head -1
}

PSF_SRC=""
PSF_SRC="$(fetch_from_deb \
    'https://deb.debian.org/debian/pool/main/t/terminus-font/terminus-font_4.48-4_all.deb' \
    'Debian terminus-font 4.48-4')"
if [ -n "$PSF_SRC" ]; then
    case "$PSF_SRC" in
        *.gz) gunzip -k -f "$PSF_SRC" 2>/dev/null; PSF_SRC="${PSF_SRC%.gz}" ;;
    esac
    cp "$PSF_SRC" "$DEST"
    echo "[OK] copied $PSF_SRC -> $DEST"
    exit 0
fi

echo "ERROR: could not obtain ter-u16n.psf"
exit 1
