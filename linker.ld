/* linker.ld - Nova OS 阶段一：内核链接脚本
 *
 * 内核高半区基址：0xFFFFFFFF80000000（用户指定）
 * SIZEOF_HEADERS 使第一个 PT_LOAD 段从文件偏移 0 开始
 * （Limine 协议要求，避免 ELF 程序头被覆盖）。
 *
 * 关键段：
 *   .requests*         - Limine 协议请求标记与请求结构（boot.S/kernel.c 声明）
 *   /DISCARD/          - 丢弃不需要的段（.eh_frame 等）
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

KERNEL_BASE = 0xFFFFFFFF80000000;

SECTIONS
{
    . = KERNEL_BASE + SIZEOF_HEADERS;

    /* Limine 请求段必须放在最前：Limine 物理扫描范围是
     * image_size_before_bss（.bss 之前的镜像大小）。若 .requests 落在
     * .bss 之后（如大体积 .rodata blob 把布局往后推），Limine 扫描不到
     * marker，framebuffer 等请求全部无响应（"no framebuffer"）。 */
    .requests : ALIGN(16) {
        KEEP(*(.requests_start_marker))
        KEEP(*(.requests))
        KEEP(*(.requests_end_marker))
    }

    .text : ALIGN(4K) {
        *(.text*)
        *(.user_text*)      /* 阶段十二：用户态演示程序（位置无关，运行时拷贝到用户地址） */
    }

    .rodata : ALIGN(4K) {
        *(.rodata*)
    }

    .data : ALIGN(4K) {
        *(.data*)
    }

    .bss : ALIGN(4K) {
        *(COMMON)
        *(.bss*)
    }

    /* 内核镜像虚拟结束地址（vmm 高半区映射按整个镜像大小） */
    _kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note*)
        *(.comment)
        *(.symtab)
        *(.strtab)
        *(.shstrtab)
    }
}
