#!/usr/bin/env bash
# 从 Debian/Ubuntu 包提取 VGA 8x16 PSF 字体
cd /tmp
for pkg in kbd console-setup; do
    echo "=== apt download $pkg ==="
    apt-get download "$pkg" 2>&1 | tail -1
    deb=$(ls -t ${pkg}_*.deb 2>/dev/null | head -1)
    if [ -n "$deb" ]; then
        echo "got $deb"
        dpkg-deb -x "$deb" /tmp/fonts-x
        find /tmp/fonts-x -name "*.psf*" 2>/dev/null | head -5
        find /tmp/fonts-x -name "*VGA*" 2>/dev/null | head -5
        break
    fi
done
