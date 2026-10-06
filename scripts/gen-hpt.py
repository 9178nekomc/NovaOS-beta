#!/usr/bin/env python3
"""scripts/gen-hpt.py - Nova v2: 生成嵌入式 HPT 软件仓库 blob

扫描 build/repo/*.nvp（我们的包内容），打包成单文件 blob 供内核
incbin 嵌入。blob 格式（仿 HBOS hpt_repo_seed.c，全部小端）：
    [u32 count]
    count * { [u16 name_len][name][u32 data_len][data] }
name 相对仓库根，如 "packages/Packages" 或 "packages/pool/calc.hax"。

构建期把仓库嵌入内核，启动时零拷贝映射到 VFS /packages；
用户仍可切换外部 server 源（后续 app add）。
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.join(ROOT, 'build', 'repo')
OUT = os.path.join(ROOT, 'build', 'hpt-repo.bin')


def main():
    if not os.path.isdir(REPO):
        print('[gen-hpt] build/repo missing - run scripts/mk-repo.sh first')
        sys.exit(1)

    entries = []
    for f in sorted(os.listdir(REPO)):
        if f.endswith('.nvp'):
            p = os.path.join(REPO, f)
            data = open(p, 'rb').read()
            # name 相对仓库根（内核 hpt_init 会拼 HPT_PKG_DIR/ 前缀）：
            #   "Packages" 清单 + "pool/<name>.hax"
            name = 'pool/' + f[:-4] + '.hax'
            entries.append((name, data))

    # Packages 清单：每行一个包名
    pkgs = sorted(f[:-4] for f in os.listdir(REPO) if f.endswith('.nvp'))
    entries.insert(0, ('Packages', ('\n'.join(pkgs) + '\n').encode()))

    buf = struct.pack('<I', len(entries))
    for name, data in entries:
        nb = name.encode('utf-8')
        buf += struct.pack('<H', len(nb)) + nb
        buf += struct.pack('<I', len(data)) + data

    with open(OUT, 'wb') as f:
        f.write(buf)
    print('[gen-hpt] %s: %d entries, %d bytes' % (OUT, len(entries), len(buf)))
    for name, data in entries:
        print('  %s: %dB' % (name, len(data)))


if __name__ == '__main__':
    main()
