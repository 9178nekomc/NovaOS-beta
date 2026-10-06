#!/usr/bin/env bash
# scripts/fetch-bdf-and-convert.sh - 下载 ter-u16n.bdf 并转 PSF2（开发用）
# 产物：src/graphics/font/ter-u16n.psf（PSF2，magic 0x864ab572）
set -euo pipefail

cd "$(dirname "$0")/.."
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

URLS=(
  "https://raw.githubusercontent.com/chromiumos/platform/frecon/39b37e5de8a68afda9c4b593f36e3f2562852df2/ter-u16n.bdf"
  "https://raw.githubusercontent.com/Protesilaos/terminus-font/master/terminus-font-4.49/ter-u16n.bdf"
)

OK=0
for u in "${URLS[@]}"; do
  echo "[Nova] trying: $u"
  if curl -sSL -m 40 -o "$TMP/f.bdf" "$u" && head -c 40 "$TMP/f.bdf" | grep -q STARTFONT; then
    OK=1
    break
  fi
done

if [ "$OK" != "1" ]; then
  echo "ERROR: could not download ter-u16n.bdf"
  exit 1
fi

echo "[Nova] converting BDF -> PSF2 (bdf2psf --fb)"
bdf2psf --fb "$TMP/f.bdf" \
  /usr/share/bdf2psf/standard.equivalents \
  /usr/share/bdf2psf/ascii.set \
  256 "$TMP/f.psf" 2>&1 | head -5 || true

if ! python3 scripts/psf2info.py "$TMP/f.psf" 2>/dev/null; then
  echo "[Nova] bdf2psf produced PSF1, converting to PSF2 ..."
  python3 scripts/psf1-to-psf2.py "$TMP/f.psf" "$TMP/f2.psf"
  mv "$TMP/f2.psf" "$TMP/f.psf"
fi

mkdir -p kernel/font
cp "$TMP/f.psf" src/graphics/font/ter-u16n.psf
echo "[OK] src/graphics/font/ter-u16n.psf ready"
python3 scripts/psf2info.py src/graphics/font/ter-u16n.psf
