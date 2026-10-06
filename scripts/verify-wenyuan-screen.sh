#!/usr/bin/env bash
# scripts/verify-wenyuan-screen.sh - 快速验证：启动后截取 wenyuan 欢迎行
# 用法: bash scripts/verify-wenyuan-screen.sh
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD="build"
SERIAL="/tmp/nova-wy-serial.log"
rm -f "$SERIAL" "$BUILD"/wy-shot*.ppm

[ -f "$BUILD/nova.iso" ] || make iso
# 挂测试盘（ext2 探测需要；否则无盘时 ext2 探测除零）
bash scripts/prepare-ext2-disk.sh >/dev/null 2>&1 || true

OVMF=""
for c in /usr/share/ovmf/OVMF_CODE.fd /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF_CODE.fd; do
    [ -f "$c" ] && OVMF="$c" && break
done
[ -n "$OVMF" ] || { echo "no OVMF"; exit 1; }

CMDS="sleep 60
echo 'screendump $BUILD/wy-shot0.ppm'
sleep 3
echo 'screendump $BUILD/wy-shot1.ppm'
echo 'quit'"

timeout -k 5 120 bash -c "( $CMDS ) | qemu-system-x86_64 -M q35 -m 512M -smp 2 \
    -no-reboot -cdrom $BUILD/nova.iso -bios $OVMF \
    -device piix4-ide,id=ide0 \
    -drive file=$BUILD/test.img,format=raw,if=none,id=disk0 \
    -device ide-hd,drive=disk0,bus=ide0.0 \
    -vnc 127.0.0.1:0 -serial file:$SERIAL -monitor stdio >/dev/null 2>&1" || true

pkill -f "qemu-system-x86_64.*nova.iso" 2>/dev/null || true

echo "=== serial: wenyuan / welcome ==="
grep -aE 'wenyuan|welcome|cli:' "$SERIAL" || true

echo "=== CJK check ==="
python3 scripts/check-wenyuan-shot.py "$BUILD/wy-shot1.ppm" || true
echo "=== shots ==="
ls -la "$BUILD"/wy-shot*.ppm 2>/dev/null || true
