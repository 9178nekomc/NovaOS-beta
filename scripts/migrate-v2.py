#!/usr/bin/env python3
"""scripts/migrate-v2.py - Nova V2 directory migration (HBOS-style layout)

Moves kernel/ and boot/ sources into a layered src/ tree per
docs/ARCHITECTURE-V2.md, rewrites every relative #include in .c/.h/.S
files, updates the Makefile paths, and preserves objcopy symbol names
for the embedded TTF by redefining symbols.

USAGE: python3 scripts/migrate-v2.py [--dry-run]
Safe: creates build/pre-v2-backup/ first. --dry-run only reports.
"""
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BACKUP = os.path.join(ROOT, "build", "pre-v2-backup")

# old relative-to-root dir -> new relative-to-root dir (dirs, not files)
MAPPING = [
    ("boot",                    "src/core"),
    ("kernel/kernel.c",         "src/core/kernel.c"),      # file pair handled below
    ("kernel/include/assert.h", "src/types/assert.h"),
    ("kernel/include/errno.h",  "src/types/errno.h"),
    ("kernel/include/limine.h", "src/types/limine.h"),
    ("kernel/include/string.h", "src/types/string.h"),
    ("kernel/lib",              "src/lib"),
    ("kernel/gdt",              "src/core/gdt"),
    ("kernel/idt",              "src/core/idt"),
    ("kernel/pic",              "src/core/pic"),
    ("kernel/lapic",            "src/core/lapic"),
    ("kernel/smp",              "src/core/smp"),
    ("kernel/timer",            "src/core/timer"),
    ("kernel/mm",               "src/core/mm"),
    ("kernel/sched",            "src/core/sched"),
    ("kernel/syscall",          "src/core/syscall"),
    ("kernel/user",             "src/user"),
    ("kernel/fs",               "src/fs"),
    ("kernel/mbr",              "src/fs/mbr"),
    ("kernel/ext2",             "src/fs/ext2"),
    ("kernel/ata",              "src/drivers/ata"),
    ("kernel/pci",              "src/drivers/pci"),
    ("kernel/net",              "src/net"),
    ("kernel/nvp",              "src/tools/nvp"),
    ("kernel/install",          "src/tools/install"),
    ("kernel/font",             "src/graphics/font"),
    ("kernel/terminal",         "src/graphics/terminal"),
    ("kernel/input",            "src/input"),
    ("kernel/cli",              "src/shell"),
]

# Files that must NOT be touched (third-party, generated, huge binaries)
SKIP = {
    "kernel/font/WenYuanSansSCVF.ttf",   # moved but never rewritten
}

def to_posix(p):
    return p.replace("\\", "/")

def map_old_to_new(old_rel):
    """Map an old path (relative to root) to its new location, or None."""
    old_rel = to_posix(old_rel)
    # exact file matches first
    for o, n in MAPPING:
        if o == old_rel and "." in os.path.basename(o):
            return n
    # directory prefix matches (longest first)
    best = None
    for o, n in MAPPING:
        if "." in os.path.basename(o):
            continue  # file pair
        if old_rel == o or old_rel.startswith(o + "/"):
            if best is None or len(o) > len(best[0]):
                best = (o, n)
    if best:
        o, n = best
        rest = old_rel[len(o):]  # "/sub/file.c"
        return n + rest
    return None

def build_lookup():
    """old_rel -> new_rel for every existing file under kernel/ and boot/.
    After migration (kernel/ gone), walk src/ instead and invert the map."""
    lookup = {}
    # inverse: new dir -> old dir
    dir_inv = {}
    for o, n in MAPPING:
        if "." in os.path.basename(o):
            continue
        dir_inv[n] = o
    for old_dir in ("kernel", "boot"):
        if os.path.isdir(os.path.join(ROOT, old_dir)):
            for dirpath, dirnames, filenames in os.walk(os.path.join(ROOT, old_dir)):
                for fn in filenames:
                    full = os.path.join(dirpath, fn)
                    rel = to_posix(os.path.relpath(full, ROOT))
                    new = map_old_to_new(rel)
                    if new is not None:
                        lookup[rel] = new
    # migrated state: walk src/ and invert dir mapping
    src_root = os.path.join(ROOT, "src")
    if os.path.isdir(src_root):
        for dirpath, dirnames, filenames in os.walk(src_root):
            for fn in filenames:
                full = os.path.join(dirpath, fn)
                newrel = to_posix(os.path.relpath(full, ROOT))
                # find the longest new-dir prefix in dir_inv
                best = None
                for n, o in dir_inv.items():
                    if newrel == n or newrel.startswith(n + "/"):
                        if best is None or len(n) > len(best[0]):
                            best = (n, o)
                if best:
                    n, o = best
                    oldrel = o + newrel[len(n):]
                    lookup.setdefault(oldrel, newrel)
    return lookup

INCLUDE_RE = re.compile(r'^(\s*#\s*include\s*)"([^"]+)"')

def old_base_of(new_rel):
    """Invert the mapping for a dir: given a NEW file path, return the OLD
    directory path so relative includes can be resolved in old coordinates."""
    for o, n in sorted(MAPPING, key=lambda x: -len(x[0])):
        if "." in os.path.basename(o):
            continue
        if new_rel == n or new_rel.startswith(n + "/"):
            rest = new_rel[len(n):]
            return o + rest
    return None

def rewrite_file(path, lookup, dry):
    """Rewrite relative includes in one source file. Returns change count.

    Resolve each include in OLD coordinates (old dir of this file + inc),
    map the target through the lookup table to its NEW location, then emit
    a relative path from the NEW file dir.
    """
    changes = 0
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    out = []
    newbase = os.path.dirname(os.path.abspath(path))
    newrel_dir = to_posix(os.path.relpath(newbase, ROOT))
    oldbase = old_base_of(newrel_dir)
    if oldbase is None:
        oldbase = ""  # root-level file (unexpected)
    for ln in lines:
        m = INCLUDE_RE.match(ln)
        if not m:
            out.append(ln)
            continue
        inc = m.group(2)
        if inc.startswith("/") or (":" in inc):
            out.append(ln)
            continue
        # resolve in OLD coordinates: old dir of this file + include
        old_target = to_posix(os.path.normpath(os.path.join(oldbase, inc)))
        new = lookup.get(old_target)
        if new is None:
            out.append(ln)  # external / same-dir / not in mapping - keep
            continue
        newabs = os.path.join(ROOT, *new.split("/"))
        newrel = to_posix(os.path.relpath(newabs, newbase))
        newline = m.group(1) + '"' + newrel + '"\n'
        if newline != ln:
            changes += 1
        out.append(newline)
    if changes and not dry:
        with open(path, "w", encoding="utf-8") as f:
            f.writelines(out)
    return changes

def main():
    dry = "--dry-run" in sys.argv
    only_includes = "--includes-only" in sys.argv
    if not dry and not only_includes:
        if os.path.exists(BACKUP):
            shutil.rmtree(BACKUP)
        shutil.copytree(os.path.join(ROOT, "kernel"), os.path.join(BACKUP, "kernel"))
        shutil.copytree(os.path.join(ROOT, "boot"), os.path.join(BACKUP, "boot"))
        print("[migrate] backup ->", BACKUP)

    lookup = build_lookup()
    print(f"[migrate] lookup: {len(lookup)} files mapped")

    # 1. move files
    if not dry and not only_includes:
        src_root = os.path.join(ROOT, "src")
        os.makedirs(src_root, exist_ok=True)
        moved = set()
        for old_rel, new_rel in sorted(lookup.items()):
            old_abs = os.path.join(ROOT, *old_rel.split("/"))
            new_abs = os.path.join(ROOT, *new_rel.split("/"))
            os.makedirs(os.path.dirname(new_abs), exist_ok=True)
            shutil.move(old_abs, new_abs)
            moved.add(old_rel)
        print(f"[migrate] moved {len(moved)} files")

    # 2. rewrite includes in moved .c/.h/.S files
    total = 0
    nfiles = 0
    for old_rel, new_rel in sorted(lookup.items()):
        if not new_rel.endswith((".c", ".h", ".S", ".asm")):
            continue
        path = os.path.join(ROOT, *new_rel.split("/"))
        if not os.path.exists(path):
            continue
        c = rewrite_file(path, lookup, dry)
        total += c
        if c:
            nfiles += 1
    print(f"[migrate] rewrote {total} includes in {nfiles} files")

    # 2b. kernel.c special case: its includes are relative to the old
    #     kernel/ root (e.g. "font/font.h"), now at src/core/kernel.c.
    #     Resolve each via the lookup table (old rel -> new rel).
    kc = os.path.join(ROOT, "src", "core", "kernel.c")
    if os.path.exists(kc) and not dry:
        with open(kc, "r", encoding="utf-8") as f:
            klines = f.readlines()
        kout = []
        kbase = os.path.dirname(kc)
        for ln in klines:
            m = INCLUDE_RE.match(ln)
            if m:
                inc = m.group(2)
                target = os.path.normpath(os.path.join(kbase, inc))
                if not os.path.exists(target):
                    # try as old kernel/-root-relative include
                    old_key = "kernel/" + inc
                    new = lookup.get(old_key)
                    if new is None:
                        # bare header like "uart.h" might live in kernel/lib
                        new = lookup.get("kernel/lib/" + inc)
                    if new is not None:
                        newabs = os.path.join(ROOT, *new.split("/"))
                        newrel = to_posix(os.path.relpath(newabs, kbase))
                        kout.append(m.group(1) + '"' + newrel + '"\n')
                        continue
            kout.append(ln)
        with open(kc, "w", encoding="utf-8") as f:
            f.writelines(kout)
        print("[migrate] kernel.c includes rewritten")

    # 3. Makefile path updates
    mk = os.path.join(ROOT, "Makefile")
    if os.path.exists(mk) and not dry and not only_includes:
        with open(mk, "r", encoding="utf-8") as f:
            content = f.read()
        # exact special cases first (kernel.c / boot.S have no subdir)
        content = content.replace("$(BUILD)/kernel/kernel.o",
                                  "$(BUILD)/src/core/kernel.o")
        content = content.replace("kernel/kernel.c", "src/core/kernel.c")
        content = content.replace("$(BUILD)/boot/boot.o",
                                  "$(BUILD)/src/core/boot.o")
        content = content.replace("boot/boot.S", "src/core/boot.S")
        # dir pairs (longest first)
        dir_map = [(o, n) for o, n in MAPPING if "." not in os.path.basename(o)]
        dir_map.sort(key=lambda x: -len(x[0]))
        for o, n in dir_map:
            content = content.replace("kernel/" + os.path.basename(o) + "/",
                                      n + "/")
            content = content.replace("kernel/" + os.path.basename(o) + " ",
                                      n + " ")
            content = content.replace("kernel/" + os.path.basename(o) + "\t",
                                      n + "\t")
            content = content.replace("kernel/" + os.path.basename(o) + ".",
                                      n + ".")
        # CFLAGS include dir and fallback prefix
        content = content.replace("-Ikernel/include", "-Isrc/types")
        content = content.replace("kernel/font/WenYuanSansSCVF.ttf",
                                  "src/graphics/font/WenYuanSansSCVF.ttf")
        content = content.replace("$(BUILD)/kernel/", "$(BUILD)/src/")
        with open(mk, "w", encoding="utf-8") as f:
            f.write(content)
        print("[migrate] Makefile updated")

        # 5. wenyuan.c extern symbols (objcopy -I binary embeds the path)
        wy = os.path.join(ROOT, "src", "graphics", "font", "wenyuan.c")
        if os.path.exists(wy):
            with open(wy, "r", encoding="utf-8") as f:
                c = f.read()
            c = c.replace("_binary_kernel_font_WenYuanSansSCVF_ttf",
                          "_binary_src_graphics_font_WenYuanSansSCVF_ttf")
            with open(wy, "w", encoding="utf-8") as f:
                f.write(c)
            print("[migrate] wenyuan.c extern symbols updated")

        # 6. helper scripts referencing kernel/ paths (comments + code)
        for sp in ("scripts/gen-font.py", "scripts/nvp-pack.py",
                   "scripts/dump-shot-rows.py", "scripts/ocr-boot.py",
                   "scripts/verify-cli.py", "scripts/verify-echo.py",
                   "scripts/verify-glyph.py", "scripts/fetch-ter-u16n.sh",
                   "scripts/fetch-bdf-and-convert.sh", "scripts/ft-check.sh",
                   "scripts/check-kprobe.sh", "scripts/check-strings.sh",
                   "scripts/marker-dis.sh", "scripts/test-qemu.sh"):
            p = os.path.join(ROOT, sp)
            if not os.path.exists(p):
                continue
            with open(p, "r", encoding="utf-8", errors="replace") as f:
                c = f.read()
            o = c
            for oo, nn in dir_map:
                c = c.replace("kernel/" + os.path.basename(oo) + "/", nn + "/")
            c = c.replace("kernel/kernel.c", "src/core/kernel.c")
            c = c.replace("kernel/include/", "src/types/")
            if c != o:
                with open(p, "w", encoding="utf-8") as f:
                    f.write(c)
                print(f"[migrate] {sp} updated")

    # 4. linker.ld / limine.conf if they mention paths
    for extra in ("linker.ld",):
        p = os.path.join(ROOT, extra)
        if os.path.exists(p) and not dry:
            with open(p, "r", encoding="utf-8") as f:
                c = f.read()
            if "kernel/" in c or "boot/" in c:
                with open(p, "w", encoding="utf-8") as f:
                    f.write(c.replace("kernel/", "src/"))
                print(f"[migrate] {extra} updated")

    print("[migrate] DONE" if not dry else "[migrate] DRY-RUN, no changes")

if __name__ == "__main__":
    main()
