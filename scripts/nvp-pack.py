#!/usr/bin/env python3
# scripts/nvp-pack.py - Nova OS 阶段十七：.nvp 包打包工具
#
# 用法：python3 scripts/nvp-pack.py <目录> <输出.nvp>
# 把目录下所有文件（非递归）打包成 NVP1 归档。
#
# 布局（与内核 src/tools/nvp/nvp.c 一致，全部小端）：
#   [0,16)      header: magic "NVP1" | version=1 | count | total_size
#   [16,16+N*16) entries[N]: name_offset | data_offset | size | crc32
#   之后        文件名字符串（NUL 结尾，按条目顺序）
#   之后        数据区
#   name/data offset 均为相对包首偏移
import os
import struct
import sys

POLY = 0xEDB88320

def crc32(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for b in data:
        crc = (crc ^ b) & 0xFFFFFFFF
        for _ in range(8):
            crc = (crc >> 1) ^ POLY if crc & 1 else crc >> 1
    return crc ^ 0xFFFFFFFF

def main():
    if len(sys.argv) != 3:
        print("usage: nvp-pack.py <dir> <out.nvp>")
        sys.exit(1)
    src_dir, out_path = sys.argv[1], sys.argv[2]

    files = []
    for name in sorted(os.listdir(src_dir)):
        p = os.path.join(src_dir, name)
        if os.path.isfile(p):
            files.append((name, open(p, 'rb').read()))
    if not files:
        print("no files in", src_dir)
        sys.exit(1)

    n = len(files)
    # 名字区：header(16) + entries(16n)
    names_off = 16 + 16 * n
    names = b''
    for name, _ in files:
        names += name.encode('utf-8') + b'\x00'
    data_off = names_off + len(names)

    # 构建条目（先用占位 offset，再回填）
    entries = []
    cur = data_off
    for name, content in files:
        entries.append([0, cur, len(content), crc32(content)])
        cur += len(content)

    # 回填 name_offset（名字区起始 + 逐个名字长度累加）
    noff = names_off
    for i, (name, _) in enumerate(files):
        entries[i][0] = noff
        noff += len(name.encode('utf-8')) + 1

    total = cur
    buf = struct.pack('<IHHII', 0x3150564E, 1, n, total, 0)   # 16 字节头
    for e in entries:
        buf += struct.pack('<IIII', *e)
    buf += names
    for _, content in files:
        buf += content

    with open(out_path, 'wb') as f:
        f.write(buf)
    print(f"[nvp-pack] {out_path}: {n} files, {total} bytes")
    for name, content in files:
        print(f"  {name}: {len(content)}B crc=0x{crc32(content):08x}")

if __name__ == '__main__':
    main()
