#!/usr/bin/env bash
cd "$(dirname "$0")/.."
BUILD="build"
cp "$BUILD/hd512.img" /tmp/verify-dual.img
( sleep 28; echo quit ) | timeout -k 5 55 qemu-system-x86_64 -M q35 -m 512M -smp 1 -no-reboot \
    -accel tcg,thread=multi \
    -boot order=c -net none \
    -device piix4-ide,id=ide0 \
    -drive "file=/tmp/verify-dual.img,format=raw,if=none,id=disk0,cache=unsafe" \
    -device ide-hd,drive=disk0,bus=ide0.0 \
    -vnc 127.0.0.1:1 -serial file:$BUILD/verify-dual.log -monitor stdio >/dev/null 2>&1
echo "=== MBR + ext2 挂载 ==="
grep -a 'MBR:\|ext2:\|multi-disk' "$BUILD/verify-dual.log" | head -12
echo "=== shell ==="
grep -aq 'nova:/\$' "$BUILD/verify-dual.log" && echo "SHELL OK"
