#!/usr/bin/env python3
# scripts/fetch-ter-u16n.py - 获取 PSF2 格式 Terminus 8x16 字体
#
# 策略：
#   1. 从 deb.debian.org 拉取 terminus-font 包列表，下载最新 .deb，解包找 ter-u16n.psf
#   2. 若包内是 PSF1（magic 0x0436），或找不到，用 bdf2psf 从官方 BDF 源生成 PSF2
#
# 输出：kernel/font/ter-u16n.psf（必须是 PSF2，magic 0x864ab572）
import io
import os
import re
import struct
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(ROOT, "kernel", "font", "ter-u16n.psf")

PSF2_MAGIC = 0x864ab572
PSF1_MAGIC = 0x0436


def fetch(url, binary=True):
    req = urllib.request.Request(url, headers={"User-Agent": "nova-build/1.0"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def check_psf(data):
    if len(data) < 4:
        return None
    m = struct.unpack_from("<I", data, 0)[0]
    if m == PSF2_MAGIC:
        return "psf2"
    if (data[0], data[1]) == (0x36, 0x04):
        return "psf1"
    return None


def extract_deb(data):
    """解包 .deb（ar 归档）：data.tar.xz/zst -> 内存 tar 流"""
    idx = 0
    while idx < len(data):
        # ar 头 60 字节
        if data[idx:idx + 2] != b"!<":
            break
        name = data[idx + 0:idx + 16].decode().strip()
        size = int(data[idx + 48:idx + 58].decode().strip())
        start = idx + 60
        blob = data[start:start + size]
        if name.startswith("data.tar"):
            if name.endswith(".xz"):
                import lzma
                return tarfile.open(fileobj=io.BytesIO(lzma.decompress(blob)))
            if name.endswith(".zst"):
                try:
                    import zstandard
                except ImportError:
                    return None
                return tarfile.open(fileobj=io.BytesIO(zstandard.ZstdDecompressor().decompress(blob)))
            if name.endswith(".gz"):
                import gzip
                return tarfile.open(fileobj=io.BytesIO(gzip.decompress(blob)))
        idx = start + size
    return None


def from_debian():
    listing = fetch("https://deb.debian.org/debian/pool/main/t/terminus-font/")
    debs = sorted(set(re.findall(r'terminus-font_[^"<>\s]+_all\.deb', listing.decode())))
    if not debs:
        return None
    url = "https://deb.debian.org/debian/pool/main/t/terminus-font/" + debs[-1]
    print(f"  deb: {debs[-1]}")
    tf = extract_deb(fetch(url))
    if tf is None:
        return None
    for m in tf.getmembers():
        if m.name.endswith("ter-u16n.psf") or m.name.endswith("ter-u16n.psf.gz"):
            data = tf.extractfile(m).read()
            if m.name.endswith(".gz"):
                import gzip
                data = gzip.decompress(data)
            if check_psf(data) == "psf2":
                return data
    return None


def from_bdf():
    """官方 BDF 源 -> bdftopsf(psftools) 或 bdf2psf -> PSF2"""
    urls = [
        # chromiumos frecon 仓库（googlesource，?format=TEXT 返回 base64）
        "https://chromium.googlesource.com/chromiumos/platform/frecon/+/"
        "39b37e5de8a68afda9c4b593f36e3f2562852df2/ter-u16n.bdf?format=TEXT",
        "https://raw.githubusercontent.com/Protesilaos/terminus-font/master/"
        "terminus-font-4.49/ter-u16n.bdf",
    ]
    for url in urls:
        try:
            print(f"  bdf url: {url}")
            raw = fetch(url)
            if b"STARTFONT" in raw[:4096] or raw[:1] == b" ":
                data = raw  # 已是纯文本 BDF
            else:
                import base64
                data = base64.b64decode(raw)
            if b"STARTFONT" not in data[:4096]:
                continue
            tmp = os.path.join(ROOT, "build", "ter-u16n.bdf")
            os.makedirs(os.path.dirname(tmp), exist_ok=True)
            with open(tmp, "wb") as f:
                f.write(data)
            if subprocess.run(["which", "bdftopsf"], capture_output=True).returncode == 0:
                subprocess.run(["bdftopsf", tmp, tmp + ".psf"], check=True)
            else:
                subprocess.run(
                    ["bdf2psf", "--fb", tmp,
                     "/usr/share/bdf2psf/standard.equivalents",
                     "/usr/share/bdf2psf/control.equivalents",
                     "256", tmp + ".psf"], check=True)
            out = open(tmp + ".psf", "rb").read()
            if check_psf(out) == "psf2":
                return out
            print("  converted font is not PSF2, trying next source")
        except Exception as e:
            print(f"  bdf failed: {e}")
    return None


def main():
    data = None
    print("[Nova] fetching ter-u16n.psf (PSF2) ...")
    print("[1/2] trying Debian terminus-font package")
    try:
        data = from_debian()
    except Exception as e:
        print(f"  debian package fetch failed: {e}")
    if data is None:
        print("[2/2] Debian package not PSF2; trying BDF conversion")
        try:
            data = from_bdf()
        except Exception as e:
            print(f"  bdf conversion failed: {e}")
    if data is None:
        print("ERROR: could not obtain a PSF2 ter-u16n font")
        sys.exit(1)
    os.makedirs(os.path.dirname(DEST), exist_ok=True)
    with open(DEST, "wb") as f:
        f.write(data)
    magic, ver, hsize, flags, ng, bpg, h, w = struct.unpack_from("<IIIIIIII", data, 0)
    print(f"[OK] {DEST} ({len(data)} bytes) PSF2 {w}x{h} {ng} glyphs")
    assert check_psf(data) == "psf2"


if __name__ == "__main__":
    main()
