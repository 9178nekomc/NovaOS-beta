#!/usr/bin/env bash
cd "$(dirname "$0")/.."
dd if=/tmp/nova-install.img of=/tmp/partx.img bs=512 skip=2048 status=none
echo "=== block 1029 (root dir) ==="
dd if=/tmp/partx.img bs=1024 skip=1029 count=1 2>/dev/null | xxd | head -8
echo "=== inode table block 5 (inode 2 at +128) ==="
dd if=/tmp/partx.img bs=1024 skip=5 count=1 2>/dev/null | xxd -l 128 -s 128
echo "=== superblock fields ==="
python3 - <<'EOF'
import struct
d = open('/tmp/partx.img','rb').read()
sb = d[1024:1024+1024]
def u32(o): return struct.unpack_from('<I', sb, o)[0]
def u16(o): return struct.unpack_from('<H', sb, o)[0]
print('magic', hex(u16(56)), 'blocks', u32(4), 'ipg', u32(40), 'bpg', u32(32), 'inodes', u32(0))
print('first_data_block', u32(20), 'log_blksz', u32(24), 'inode_size', u16(88))
print('free_blocks', u32(12), 'free_inodes', u32(16))
print('feature_compat', hex(u32(92)), 'rev', u32(76))
print('block_bitmap(grp0)', u32(1024+8), 'inode_bitmap', u32(1024+12), 'inode_table', u32(1024+16))
EOF
