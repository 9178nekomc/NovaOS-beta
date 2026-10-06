#!/usr/bin/env bash
cd "$(dirname "$0")/.."
ls -la /usr/share/ovmf/ 2>/dev/null | head -8
OVMF="/usr/share/ovmf/OVMF_CODE.fd"
[ -f "$OVMF" ] || OVMF="/usr/share/ovmf/OVMF.fd"
echo "using $OVMF"
( sleep 35; echo quit ) | timeout -k 5 60 qemu-system-x86_64 -M q35 -m 512M -smp 1 -no-reboot \
    -accel tcg,thread=multi \
    -cdrom build/nova.iso -bios "$OVMF" \
    -device piix4-ide,id=ide0 \
    -drive file=/tmp/hd512.img,format=raw,if=none,id=disk0,cache=unsafe \
    -device ide-hd,drive=disk0,bus=ide0.0 \
    -vnc 127.0.0.1:0 -serial file:build/quickboot.log -monitor stdio >/dev/null 2>&1
echo "=== markers ==="
grep -a 'kprobe\|vmm test\|kmalloc init' build/quickboot.log | head -6
