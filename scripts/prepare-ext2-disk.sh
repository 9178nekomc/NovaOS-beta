#!/usr/bin/env bash
# scripts/prepare-ext2-disk.sh - 生成带 MBR + ext2 分区的测试盘（WSL）
#
# 输出：build/test.img（64MB raw）
#   - MBR：分区 0 = type 0x83（Linux），起始 LBA 2048，大小 32MB
#   - 分区内：ext2（1KB 块）+ 测试文件 hello.txt
#
# 依赖：mkfs.ext2、debugfs（e2fsprogs）
set -euo pipefail
cd "$(dirname "$0")/.."

DISK="build/test.img"
PART_SZ_MB=30                     # 分区大小（30MB）
MBR_PART_LBA=2048                 # 分区起始 LBA（与内核 mbr_build_sample 一致）
IMG="/tmp/nova-ext2-part.img"
HELLO="/tmp/nova-ext2-hello.txt"

echo "[ext2-prepare] building $DISK ..."

# 1. 空盘 + MBR（分区 0：0x83 @ LBA2048，64MB）
truncate -s 64M "$DISK"
python3 - "$DISK" <<'EOF'
import struct, sys
disk = sys.argv[1]
mbr = bytearray(512)
# part0: type 0x83, start LBA 2048, 128MiB sectors (1<<17)
part = struct.pack('<B3sB3sII', 0x80, b'\0\0\0', 0x83, b'\0\0\0', 2048, 1 << 17)
mbr[446:462] = part
mbr[510:512] = b'\x55\xAA'
with open(disk, 'r+b') as f:
    f.write(bytes(mbr))
print('  MBR written (part0 0x83 @ LBA2048)')
EOF

# 2. ext2 分区镜像（1KB 块）+ 测试文件
dd if=/dev/zero of="$IMG" bs=1M count=$PART_SZ_MB status=none
mkfs.ext2 -q -b 1024 -F "$IMG"
echo "Hello from Nova ext2! [pid-demo]" > "$HELLO"
debugfs -w -R "write $HELLO hello.txt" "$IMG" >/dev/null 2>&1

# 阶段十七：打包 demo.nvp 并写入 ext2 分区
NVPDIR="/tmp/nova-nvp-demo"
rm -rf "$NVPDIR"
mkdir -p "$NVPDIR"
echo "Nova package format demo" > "$NVPDIR/readme.txt"
echo "Hello from .nvp!" > "$NVPDIR/hello.txt"
echo "1.0.0" > "$NVPDIR/version.txt"
python3 scripts/nvp-pack.py "$NVPDIR" build/demo.nvp >/dev/null
debugfs -w -R "write build/demo.nvp demo.nvp" "$IMG" >/dev/null 2>&1
rm -rf "$NVPDIR"

# 阶段十九：构建软件仓库目录（host HTTP server 服务 build/repo/）
#   index.nvp   —— NVP 包，内含 INDEX 清单（每行一个包名）
#   util.nvp    —— 仓库包 1（readme + version）
#   webdemo.nvp —— 仓库包 2（index.html + version，ext2 中不存在）
#   calc.nvp    —— 仓库包 3（calc 计算器命令的软件包：version/readme/ops）
REPO="build/repo"
rm -rf "$REPO" /tmp/nova-repo-src
mkdir -p "$REPO" /tmp/nova-repo-src/util /tmp/nova-repo-src/webdemo \
         /tmp/nova-repo-src/calc
printf 'util\nwebdemo\ncalc\n' > /tmp/nova-repo-src/INDEX
python3 scripts/nvp-pack.py /tmp/nova-repo-src "$REPO/index.nvp" >/dev/null
echo "Nova utility package (repo)" > /tmp/nova-repo-src/util/readme.txt
echo "2.0.0" > /tmp/nova-repo-src/util/version.txt
python3 scripts/nvp-pack.py /tmp/nova-repo-src/util "$REPO/util.nvp" >/dev/null
printf '<html><body><h1>Hello from Nova repo!</h1></body></html>\n' \
    > /tmp/nova-repo-src/webdemo/index.html
echo "3.1.4" > /tmp/nova-repo-src/webdemo/version.txt
python3 scripts/nvp-pack.py /tmp/nova-repo-src/webdemo "$REPO/webdemo.nvp" >/dev/null
echo "1.0.0" > /tmp/nova-repo-src/calc/version.txt
printf 'Nova calc: integer expression calculator.\nUsage: calc <expr>  e.g. calc (1+2)*3\n' \
    > /tmp/nova-repo-src/calc/readme.txt
echo "+ - * / ( )" > /tmp/nova-repo-src/calc/ops.txt
python3 scripts/nvp-pack.py /tmp/nova-repo-src/calc "$REPO/calc.nvp" >/dev/null
rm -rf /tmp/nova-repo-src
ls -l "$REPO"

# 3. dd 到磁盘分区偏移（2048 扇区 = 1MB）
dd if="$IMG" of="$DISK" bs=512 seek=$MBR_PART_LBA conv=notrunc status=none

rm -f "$IMG" "$HELLO"
echo "[ext2-prepare] done: MBR + ext2(hello.txt) at LBA $MBR_PART_LBA"

# ------------------------------------------------------------------
# 阶段二十（多盘）：第二块盘 build/test2.img（E: 盘）
# 同样 MBR + ext2，内容不同（data.txt），用于验证多盘切换
# ------------------------------------------------------------------
DISK2="build/test2.img"
PART_SZ_MB2=24
IMGV="/tmp/nova-ext2-part2.img"
DATA="/tmp/nova-ext2-data.txt"

echo "[ext2-prepare] building $DISK2 (second drive) ..."

truncate -s 48M "$DISK2"
python3 - "$DISK2" <<'EOF'
import struct, sys
disk = sys.argv[1]
mbr = bytearray(512)
part = struct.pack('<B3sB3sII', 0x80, b'\0\0\0', 0x83, b'\0\0\0', 2048, 1 << 16)
mbr[446:462] = part
mbr[510:512] = b'\x55\xAA'
with open(disk, 'r+b') as f:
    f.write(bytes(mbr))
print('  MBR written (part0 0x83 @ LBA2048)')
EOF

dd if=/dev/zero of="$IMGV" bs=1M count=$PART_SZ_MB2 status=none
mkfs.ext2 -q -b 1024 -F "$IMGV"
printf 'Second drive data file (E:).\nNova multi-disk test.\n' > "$DATA"
debugfs -w -R "write $DATA data.txt" "$IMGV" >/dev/null 2>&1
mkdir -p /tmp/nova-e2-dir
printf 'A directory on the second drive.\n' > /tmp/nova-e2-dir/notes.txt
debugfs -w -R "mkdir e2data" "$IMGV" >/dev/null 2>&1
debugfs -w -R "write /tmp/nova-e2-dir/notes.txt e2data/notes.txt" "$IMGV" >/dev/null 2>&1
rm -rf /tmp/nova-e2-dir "$DATA"

dd if="$IMGV" of="$DISK2" bs=512 seek=$MBR_PART_LBA conv=notrunc status=none
rm -f "$IMGV"
echo "[ext2-prepare] done: second drive $DISK2 (E: ext2, data.txt)"
