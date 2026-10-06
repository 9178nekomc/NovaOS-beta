#!/usr/bin/env bash
cd "$(dirname "$0")/.."
QEMU="${QEMU:-qemu-system-x86_64}"
OVMF_CODE="${OVMF_CODE:-/usr/share/ovmf/OVMF_CODE.fd}"
[ -f "$OVMF_CODE" ] || OVMF_CODE="/usr/share/ovmf/OVMF.fd"
BUILD="build"
ISO="$BUILD/nova.iso"
INSTALL_IMG="/tmp/nova-install.img"

rm -f "$INSTALL_IMG"
[ -f "$ISO" ] || make iso
truncate -s 64M "$INSTALL_IMG"

echo "=== phase 1: install (auto mode) ==="
CMDS="sleep 30
echo 'sendkey i'; sleep 2
echo 'sendkey n'; sleep 2
echo 'sendkey s'; sleep 2
echo 'sendkey t'; sleep 2
echo 'sendkey a'; sleep 2
echo 'sendkey l'; sleep 2
echo 'sendkey l'; sleep 2
echo 'sendkey spc'; sleep 2
echo 'sendkey 1'; sleep 2
echo 'sendkey spc'; sleep 2
echo 'sendkey y'; sleep 2
echo 'sendkey e'; sleep 2
echo 'sendkey s'; sleep 2
echo 'sendkey ret'
sleep 30
echo 'screendump build/inst1a.ppm'
sleep 30
echo 'screendump build/inst1b.ppm'
sleep 30
echo 'screendump build/inst1c.ppm'
sleep 30
echo 'screendump build/inst1d.ppm'
sleep 5
echo 'quit'"
timeout -k 5 300 bash -c "( $CMDS ) | \"$QEMU\" -M q35 -m 512M -smp 2 -no-reboot \
    -accel tcg,thread=multi \
    -cdrom \"$ISO\" -bios \"$OVMF_CODE\" \
    -device piix4-ide,id=ide0 \
    -drive \"file=$BUILD/test.img,format=raw,if=none,id=disk0,cache=unsafe\" \
    -device ide-hd,drive=disk0,bus=ide0.0 \
    -drive \"file=$INSTALL_IMG,format=raw,if=none,id=disk1,cache=unsafe\" \
    -device ide-hd,drive=disk1,bus=ide0.1 \
    -vnc 127.0.0.1:0 -serial file:$BUILD/inst1.log -monitor stdio >/dev/null 2>&1" || true
pkill -f "qemu-system-x86_64.*nova.iso" 2>/dev/null || true
grep -a 'install' "$BUILD/inst1.log" | tail -6

echo "=== partition check (debugfs) ==="
dd if="$INSTALL_IMG" of="$BUILD/part.img" bs=512 skip=2048 status=none
debugfs -R "ls -l /" "$BUILD/part.img" 2>&1 | head -10
echo "=== root dir block 1029 ==="
dd if="$BUILD/part.img" bs=1024 skip=1029 count=1 2>/dev/null | xxd | head -6
echo "=== superblock ==="
python3 - <<'EOF'
import struct
d = open('build/part.img','rb').read()
sb = d[1024:2048]
u32 = lambda o: struct.unpack_from('<I', sb, o)[0]
u16 = lambda o: struct.unpack_from('<H', sb, o)[0]
print('magic', hex(u16(56)), 'blocks', u32(4), 'inodes', u32(0))
print('ipg', u32(40), 'bpg', u32(32), 'inode_size', u16(88))
print('block_bitmap', u32(1024+8), 'inode_bitmap', u32(1024+12), 'inode_table', u32(1024+16))
EOF

echo "=== phase 2: boot from disk ==="
( sleep 25; echo "screendump $BUILD/boothd.ppm"; sleep 1; echo quit ) | \
    timeout -k 5 70 "$QEMU" -M q35 -m 512M -smp 2 -no-reboot \
        -accel tcg,thread=multi \
        -boot order=c -net none \
        -device piix4-ide,id=ide0 \
        -drive "file=$INSTALL_IMG,format=raw,if=none,id=disk0,cache=unsafe" \
        -device ide-hd,drive=disk0,bus=ide0.0 \
        -vnc 127.0.0.1:1 -serial file:$BUILD/boot1.log -monitor stdio >/dev/null 2>&1
echo "=== boot serial ==="
grep -a '\[Nova\]' "$BUILD/boot1.log" | head -8
echo "=== boot screen ==="
python3 scripts/ocr-boot.py 2>&1 | head -8
