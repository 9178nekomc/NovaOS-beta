#!/usr/bin/env bash
# 重建测试盘：删除旧盘，创建两块崭新空盘（512M + 256M）
cd "$(dirname "$0")/.."
rm -f build/test.img build/test2.img build/install.img build/inst-final.img build/part.img
rm -f /tmp/*.img
truncate -s 512M build/hd512.img
truncate -s 256M build/hd256.img
ls -la build/hd512.img build/hd256.img
echo "=== verify blank (first sectors all zero) ==="
dd if=build/hd512.img bs=512 count=1 2>/dev/null | xxd | head -2
dd if=build/hd256.img bs=512 count=1 2>/dev/null | xxd | head -2
