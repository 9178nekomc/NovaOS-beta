# Nova OS 阶段一：顶层 Makefile
#
# 目标：
#   make            - 构建 ISO（默认）
#   make iso        - 仅构建 ISO
#   make kernel     - 仅构建 kernel.elf
#   make run        - QEMU 运行（默认 UEFI，需 OVMF）
#   make run-uefi   - QEMU UEFI 启动
#   make run-bios   - QEMU BIOS(SeaBIOS) 启动
#   make clean      - 清理 build/
#
# 变量（可用环境变量覆盖）：
#   CC / LD / QEMU / OVMF_CODE / LIMINE_TAG
#
# Linux 默认值；Windows 用户请通过 WSL 构建：
#   wsl -d Ubuntu -e make iso

ARCH        ?= x86_64
KERNEL_BASE := 0xFFFFFFFF80000000

CC          ?= gcc
LD          ?= ld
OBJCOPY     ?= objcopy
QEMU        ?= qemu-system-x86_64
OVMF_CODE   ?= /usr/share/ovmf/OVMF_CODE.fd

CFLAGS  := -ffreestanding -mno-red-zone -mcmodel=large \
           -fno-stack-protector -fno-pic -fno-pie -nostdlib \
           -O2 -Wall -Wextra -g -std=gnu11 -Isrc/types
ASFLAGS := -ffreestanding -fno-pic
LDFLAGS := -nostdlib -static -T linker.ld \
           -z max-page-size=0x1000 -no-pie

BUILD   := build
ISODIR  := $(BUILD)/iso_root
KERNEL  := $(BUILD)/kernel.elf
ISO     := $(BUILD)/nova.iso
LIMINE  := limine

# FreeType（阶段二十：文渊黑体）——必须先于 OBJS 定义（OBJS 为立即展开）
FREETYPE  := third_party/freetype
FREETYPE_CFLAGS := -I$(FREETYPE)/include -DFT2_BUILD_LIBRARY \
                   -DFT_CONFIG_OPTION_DISABLE_STREAM_SUPPORT \
                   -ffreestanding -fno-builtin -O2 -w
FREETYPE_SRCS := $(FREETYPE)/src/base/ftbase.c \
                 $(FREETYPE)/src/base/ftbitmap.c \
                 $(FREETYPE)/src/base/ftinit.c \
                 $(FREETYPE)/src/base/ftmm.c \
                 $(FREETYPE)/src/truetype/truetype.c \
                 $(FREETYPE)/src/cff/cff.c \
                 $(FREETYPE)/src/sfnt/sfnt.c \
                 $(FREETYPE)/src/psaux/psaux.c \
                 $(FREETYPE)/src/pshinter/pshinter.c \
                 $(FREETYPE)/src/psnames/psnames.c \
                 $(FREETYPE)/src/smooth/smooth.c \
                 $(FREETYPE)/src/raster/raster.c \
                 $(FREETYPE)/src/autofit/autofit.c
FREETYPE_OBJS := $(patsubst $(FREETYPE)/src/%.c,$(BUILD)/third_party/%.o,$(FREETYPE_SRCS))
FONT_TTF := src/graphics/font/WenYuanSansSCVF.ttf

OBJS    := $(BUILD)/src/core/boot.o \
           $(BUILD)/src/core/kernel.o \
           $(BUILD)/src/lib/string.o \
           $(BUILD)/src/lib/printf.o \
           $(BUILD)/src/lib/uart.o \
           $(BUILD)/src/lib/panic.o \
           $(BUILD)/src/lib/rand.o \
           $(BUILD)/src/lib/klog.o \
           $(BUILD)/src/lib/libc_extra.o \
           $(BUILD)/src/graphics/font/font.o \
           $(BUILD)/src/graphics/font/font_cjk.o \
           $(BUILD)/src/graphics/font/font_cjk_bin.o \
           $(BUILD)/src/graphics/terminal/terminal.o \
           $(BUILD)/src/core/gdt/gdt.o \
           $(BUILD)/src/core/gdt/gdt_flush.o \
           $(BUILD)/src/core/gdt/tss.o \
           $(BUILD)/src/core/idt/idt.o \
           $(BUILD)/src/core/idt/isr.o \
           $(BUILD)/src/core/pic/pic.o \
           $(BUILD)/src/input/keyboard.o \
           $(BUILD)/src/core/timer/timer.o \
           $(BUILD)/src/core/mm/pmm.o \
           $(BUILD)/src/core/mm/buddy.o \
           $(BUILD)/src/core/mm/vmm.o \
           $(BUILD)/src/core/mm/kmalloc.o \
           $(BUILD)/src/core/sched/sched.o \
           $(BUILD)/src/core/sched/switch_to.o \
           $(BUILD)/src/core/lapic/lapic.o \
           $(BUILD)/src/core/smp/smp.o \
           $(BUILD)/src/core/syscall/syscall.o \
           $(BUILD)/src/user/user.o \
           $(BUILD)/src/user/user_prog.o \
           $(BUILD)/src/fs/vfs.o \
           $(BUILD)/src/fs/tmpfs.o \
           $(BUILD)/src/drivers/ata/ata.o \
           $(BUILD)/src/fs/mbr/mbr.o \
           $(BUILD)/src/fs/ext2/ext2.o \
           $(BUILD)/src/fs/ext2fs.o \
           $(BUILD)/src/tools/install/install.o \
           $(BUILD)/src/tools/install/fat.o \
           $(BUILD)/src/tools/nvp/nvp.o \
           $(BUILD)/src/tools/nvp/nvpmgr.o \
           $(BUILD)/src/tools/nvp/hpt.o \
           $(BUILD)/src/tools/nvp/hpt_seed.o \
           $(BUILD)/src/drivers/pci/pci.o \
           $(BUILD)/src/net/e1000.o \
           $(BUILD)/src/net/net.o \
           $(BUILD)/src/net/tcp.o \
           $(BUILD)/src/net/http.o \
           $(BUILD)/src/net/repo.o \
           $(BUILD)/src/shell/cli.o \
           $(BUILD)/src/shell/shell.o \
           $(BUILD)/src/shell/command.o \
           $(BUILD)/src/shell/calc.o \
           $(BUILD)/src/shell/app.o

.PHONY: all kernel iso limine-bin run run-uefi run-bios clean

all: $(ISO)

kernel: $(KERNEL)

# ---------------------------------------------------------------- FreeType 编译规则（变量定义见文件顶部）
# 裁剪模块：base/truetype/cff/sfnt/psaux/pshinter/psnames/smooth/raster/autofit。
# ftdebug.c 由 src/graphics/font/ftdebug_kernel.c 替代；ftsystem.c 用宏把
# malloc/free/realloc 映射到内核 kmalloc/kfree/krealloc。
# 注意：sfnt.c/truetype.c 等聚合编译（#include 同目录所有 .c），
# 修改任何被 include 的文件都必须触发对应 .o 重建。
SFNT_DEPS := $(FREETYPE)/src/sfnt/pngshim.c $(FREETYPE)/src/sfnt/sfdriver.c \
             $(FREETYPE)/src/sfnt/sfobjs.c $(FREETYPE)/src/sfnt/sfwoff.c \
             $(FREETYPE)/src/sfnt/sfwoff2.c $(FREETYPE)/src/sfnt/ttbdf.c \
             $(FREETYPE)/src/sfnt/ttcmap.c $(FREETYPE)/src/sfnt/ttcolr.c \
             $(FREETYPE)/src/sfnt/ttcpal.c $(FREETYPE)/src/sfnt/ttsvg.c \
             $(FREETYPE)/src/sfnt/ttgpos.c $(FREETYPE)/src/sfnt/ttkern.c \
             $(FREETYPE)/src/sfnt/ttload.c $(FREETYPE)/src/sfnt/ttmtx.c \
             $(FREETYPE)/src/sfnt/ttpost.c $(FREETYPE)/src/sfnt/ttsbit.c \
             $(FREETYPE)/src/sfnt/woff2tags.c
TT_DEPS := $(FREETYPE)/src/truetype/ttdriver.c $(FREETYPE)/src/truetype/ttgload.c \
           $(FREETYPE)/src/truetype/ttgxvar.c $(FREETYPE)/src/truetype/ttinterp.c \
           $(FREETYPE)/src/truetype/ttobjs.c $(FREETYPE)/src/truetype/ttpload.c
$(BUILD)/third_party/truetype/truetype.o: $(FREETYPE)/src/truetype/truetype.c $(TT_DEPS) \
        $(FREETYPE)/include/freetype/config/ftmodule.h \
        $(FREETYPE)/include/freetype/config/ftoption.h
	@mkdir -p $(dir $@)
	$(CC) $(FREETYPE_CFLAGS) -c $< -o $@

$(BUILD)/third_party/sfnt/sfnt.o: $(FREETYPE)/src/sfnt/sfnt.c $(SFNT_DEPS) \
        $(FREETYPE)/include/freetype/config/ftmodule.h \
        $(FREETYPE)/include/freetype/config/ftoption.h
	@mkdir -p $(dir $@)
	$(CC) $(FREETYPE_CFLAGS) -c $< -o $@

$(BUILD)/third_party/%.o: $(FREETYPE)/src/%.c \
        $(FREETYPE)/include/freetype/config/ftmodule.h \
        $(FREETYPE)/include/freetype/config/ftoption.h
	@mkdir -p $(dir $@)
	$(CC) $(FREETYPE_CFLAGS) -c $< -o $@

# 原版 ftsystem.c 因依赖 stdio 且 ftstdlib.h 会覆盖 -D 宏映射，改为
# 自写 ftsystem_kernel.c（内存分配接 kmalloc，文件流整体禁用）。
$(BUILD)/src/graphics/font/ftsystem_kernel.o: src/graphics/font/ftsystem_kernel.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(FREETYPE_CFLAGS) -c $< -o $@

# 内核侧引用 FreeType 头文件的源文件需要 include 路径
$(BUILD)/src/graphics/font/ftdebug_kernel.o: src/graphics/font/ftdebug_kernel.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(FREETYPE_CFLAGS) -c $< -o $@

$(BUILD)/src/graphics/font/wenyuan.o: src/graphics/font/wenyuan.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(FREETYPE_CFLAGS) -c $< -o $@

# Nova v2：CJK 16x16 位图字体（scripts/gen-cjk.py 从文渊 TTF 渲染，
# HZKH blob；运行时纯查表 blit，根治 FreeType 越界写坑 4.1）
$(BUILD)/font-cjk.bin: scripts/gen-cjk.py $(FONT_TTF)
	@mkdir -p $(dir $@)
	python3 scripts/gen-cjk.py $(FONT_TTF) $@

$(BUILD)/src/graphics/font/font_cjk_bin.o: $(BUILD)/font-cjk.bin
	@mkdir -p $(dir $@)
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata.cjk $< $@

# ---------------------------------------------------------------- 编译规则

# PSF2 字体头：由 scripts/gen-font.py 从 ter-u16n.psf 生成
src/graphics/font/psf2font.h: src/graphics/font/ter-u16n.psf scripts/gen-font.py
	python3 scripts/gen-font.py $< $@

$(BUILD)/src/graphics/font/font.o: src/graphics/font/psf2font.h

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# ---------------------------------------------------------------- 系统镜像嵌入（安装程序用）
# 安装内核 = 正式内核 + 嵌入的"系统镜像数据"（正式内核副本、Limine BIOS
# 引导器、limine.conf）。安装时把这些数据写到目标硬盘，之后硬盘直启。
# 两步链接：kernel-core.elf（不含 sysimg 段；仅剔除 install.o——
# install.o 引用 sysimg_kernel 等符号，只能进最终内核）-> 嵌入 ->
# 最终 kernel.elf。
# 注意：hpt_seed.o 与 sysimg_hpt.o 必须留在 core——已装系统引导的是
# core 副本，若 HPT 仓库只进最终内核，硬盘直启后 app 命令会报 0 包。
SYSIMG_DIR := $(BUILD)/sysimg
CORE_OBJS := $(filter-out $(BUILD)/src/tools/install/install.o,$(OBJS)) \
             $(BUILD)/sysimg_hpt.o

$(BUILD)/kernel-core.elf: $(CORE_OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(CORE_OBJS)

# 生成 .incbin 汇编（符号名固定，避免 objcopy 符号依赖文件路径）
$(BUILD)/sysimg_kernel.S: $(BUILD)/kernel-core.elf
	@mkdir -p $(dir $@)
	@printf '.section .rodata.sysimg,"a"\n.global sysimg_kernel_start,sysimg_kernel_end\nsysimg_kernel_start:\n.incbin "%s"\nsysimg_kernel_end:\n' "$<" > $@

$(BUILD)/sysimg_limine.S: $(LIMINE)/limine-bios.sys
	@mkdir -p $(dir $@)
	@printf '.section .rodata.sysimg,"a"\n.global sysimg_limine_start,sysimg_limine_end\nsysimg_limine_start:\n.incbin "%s"\nsysimg_limine_end:\n' "$<" > $@

$(BUILD)/sysimg_conf.S: limine.conf
	@mkdir -p $(dir $@)
	@printf '.section .rodata.sysimg,"a"\n.global sysimg_conf_start,sysimg_conf_end\nsysimg_conf_start:\n.incbin "%s"\nsysimg_conf_end:\n' "$<" > $@

$(BUILD)/sysimg_dict.S: tools/ime/dict.dat
	@mkdir -p $(dir $@)
	@printf '.section .rodata.sysimg,"a"\n.global sysimg_dict_start,sysimg_dict_end\nsysimg_dict_start:\n.incbin "%s"\nsysimg_dict_end:\n' "$<" > $@

# 官方 Limine BIOS HDD 引导数据（stage1+stage2 两半，含 MBR 0x1A4 字段）。
# 来源：limine bios-install 写入的标准 hdd 布局（见 scripts/extract-stages.sh）。
$(BUILD)/sysimg_hdd.S: $(BUILD)/official-stages/hdd-stages.bin
	@mkdir -p $(dir $@)
	@printf '.section .rodata.sysimg,"a"\n.global sysimg_hdd_start,sysimg_hdd_end\nsysimg_hdd_start:\n.incbin "%s"\nsysimg_hdd_end:\n' "$<" > $@

# Limine UEFI 引导器（ESP/EFI/BOOT/BOOTX64.EFI，自包含，走 GOP）
$(BUILD)/sysimg_uefi.S: $(LIMINE)/BOOTX64.EFI
	@mkdir -p $(dir $@)
	@printf '.section .rodata.sysimg,"a"\n.global sysimg_uefi_start,sysimg_uefi_end\nsysimg_uefi_start:\n.incbin "%s"\nsysimg_uefi_end:\n' "$<" > $@

# Nova v2：嵌入式 HPT 软件仓库（scripts/gen-hpt.py 扫描 build/repo -> blob）
$(BUILD)/hpt-repo.bin: $(wildcard $(BUILD)/repo/*.nvp) scripts/gen-hpt.py
	@mkdir -p $(dir $@)
	python3 scripts/gen-hpt.py

$(BUILD)/sysimg_hpt.S: $(BUILD)/hpt-repo.bin
	@mkdir -p $(dir $@)
	@printf '.section .rodata.sysimg,"a"\n.global sysimg_hpt_start,sysimg_hpt_end\nsysimg_hpt_start:\n.incbin "%s"\nsysimg_hpt_end:\n' "$<" > $@

$(BUILD)/sysimg_%.o: $(BUILD)/sysimg_%.S
	$(CC) $(ASFLAGS) -c $< -o $@

SYSIMG_OBJS := $(BUILD)/sysimg_kernel.o $(BUILD)/sysimg_limine.o \
               $(BUILD)/sysimg_conf.o $(BUILD)/sysimg_dict.o \
               $(BUILD)/sysimg_hdd.o $(BUILD)/sysimg_uefi.o \
               $(BUILD)/sysimg_hpt.o

$(KERNEL): $(OBJS) $(SYSIMG_OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(SYSIMG_OBJS)

# ---------------------------------------------------------------- Limine 二进制
# v8.x 仓库需先 bootstrap + configure + make 构建（gcc + nasm + mtools + autoconf）。
# 若 limine/ 下已有构建产物（scripts/clone-limine.sh 或手工构建），则直接使用。

limine-bin:
	@if [ ! -f $(LIMINE)/limine-bios-cd.bin ]; then \
		if [ -f $(LIMINE)/GNUmakefile ]; then \
			$(MAKE) -C $(LIMINE); \
		else \
			echo "ERROR: Limine binaries missing in $(LIMINE)/"; \
			echo "       Run: bash scripts/clone-limine.sh"; \
			exit 1; \
		fi \
	fi

$(LIMINE)/limine-bios-cd.bin: limine-bin ;

# ---------------------------------------------------------------- ISO 打包

$(ISODIR)/boot/limine: $(LIMINE)/limine-bios-cd.bin
	@mkdir -p $(ISODIR)/boot/limine $(ISODIR)/EFI/BOOT
	cp $(LIMINE)/limine-bios.sys       $(ISODIR)/boot/limine/
	cp $(LIMINE)/limine-bios-cd.bin    $(ISODIR)/boot/limine/
	cp $(LIMINE)/limine-uefi-cd.bin    $(ISODIR)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI           $(ISODIR)/EFI/BOOT/
	cp $(LIMINE)/BOOTIA32.EFI          $(ISODIR)/EFI/BOOT/

iso: $(KERNEL) $(ISODIR)/boot/limine
	cp $(KERNEL) limine.conf $(ISODIR)/
	xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image \
		--protective-msdos-label $(ISODIR) -o $(ISO)
	$(LIMINE)/limine bios-install $(ISO)
	@echo "[OK] $(ISO) built"

# ---------------------------------------------------------------- QEMU 运行
# OVMF 自动探测：依次尝试常见路径，找不到则报错提示

OVMF_CODE_CANDIDATES := /usr/share/ovmf/OVMF_CODE.fd \
                        /usr/share/ovmf/OVMF.fd \
                        /usr/share/OVMF/OVMF_CODE.fd \
                        $(CURDIR)/build/ovmf.fd

run-uefi: iso
	@ovmf=""; \
	for c in $(OVMF_CODE_CANDIDATES); do \
		if [ -f "$$c" ]; then ovmf="$$c"; break; fi; \
	done; \
	if [ -z "$$ovmf" ]; then \
		echo "ERROR: OVMF firmware not found. Set OVMF_CODE=<path> or install ovmf."; \
		echo "       Windows QEMU: copy WSL /usr/share/ovmf/OVMF.fd to build/ovmf.fd"; \
		exit 1; \
	fi; \
	echo "[Nova] using OVMF: $$ovmf"; \
	$(QEMU) -M q35 -m 512M -smp 4 -bios "$$ovmf" -cdrom $(ISO)

run-bios: iso
	$(QEMU) -M q35 -m 512M -smp 4 -cdrom $(ISO)

run: run-uefi

# ---------------------------------------------------------------- 清理
# 注意：只清编译产物；保留验收资产（磁盘镜像/固件副本/包仓库/测试脚本），
# 它们重建成本高（重装一次 ~10 分钟）且被 clean 误删过两次（P102）。
clean:
	rm -rf $(BUILD)/boot $(BUILD)/kernel $(BUILD)/src $(BUILD)/third_party \
	       $(BUILD)/iso_root $(BUILD)/sysimg \
	       $(BUILD)/kernel-core.elf $(BUILD)/kernel.elf $(BUILD)/nova.iso \
	       $(BUILD)/sysimg_*.o $(BUILD)/sysimg_*.S \
	       $(BUILD)/font-cjk.bin $(BUILD)/hpt-repo.bin \
	       $(BUILD)/demo.nvp $(BUILD)/*.log $(BUILD)/*.ppm $(BUILD)/*.png \
	       $(BUILD)/official-stages
	@echo "[clean] build artifacts removed (repo/disks/ovmf/scripts kept)"
