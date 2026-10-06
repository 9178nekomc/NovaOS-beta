#!/usr/bin/env bash
cd "$(dirname "$0")/.."
echo "=== extract kernel from ISO and check marker ==="
python3 - <<'EOF'
# ISO 是 ISO9660；直接读文件偏移（iso_root 构造的 ISO，kernel.elf 在根）
data = open('build/nova.iso','rb').read()
marker = b'kprobe marker 1'
pos = data.find(marker)
print('marker in ISO at:', hex(pos) if pos >= 0 else 'NOT FOUND')
EOF
echo "=== quick boot check (no install) ==="
( sleep 35; echo quit ) | timeout -k 5 60 qemu-system-x86_64 -M q35 -m 512M -smp 1 -no-reboot \
    -accel tcg,thread=multi \
    -cdrom build/nova.iso -bios /usr/share/ovmf/OVMF.fd \
    -device piix4-ide,id=ide0 \
    -drive file=/tmp/hd512.img,format=raw,if=none,id=disk0,cache=unsafe \
    -device ide-hd,drive=disk0,bus=ide0.0 \
    -vnc 127.0.0.1:0 -serial file:build/quickboot.log -monitor stdio >/dev/null 2>&1
grep -a 'kprobe\|vmm test\|kmalloc init' build/quickboot.log | head -5
