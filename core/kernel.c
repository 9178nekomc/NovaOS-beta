/* kernel/kernel.c - Nova OS 阶段四：键盘中断与 PS/2 驱动
 *
 * 职责：
 *   1. 声明 Limine v8 协议请求（引导器信息 / GOP 帧缓冲 / 内存映射 / HHDM）
 *   2. 初始化帧缓冲、PSF2 终端、串口（阶段二/三）
 *   3. gdt_init() + idt_init()（阶段三）
 *   4. pic_init()：8259A 重映射（IRQ0-15 -> int 0x20-0x2F），屏蔽全部
 *   5. keyboard_init()：取消屏蔽 IRQ1，安装键盘中断处理
 *   6. sti 开启中断
 *   7. 主循环：轮询 keyboard_getchar()，收到字符 terminal_putchar() 回显；
 *      Ctrl+Alt+Del 软重启
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <limine.h>

#include "../graphics/font/font.h"
#include "../graphics/terminal/terminal.h"
#include "gdt/gdt.h"
#include "idt/idt.h"
#include "../lib/io.h"
#include "../lib/uart.h"
#include "pic/pic.h"
#include "../input/keyboard.h"
#include "timer/timer.h"
#include "mm/pmm.h"
#include "mm/buddy.h"
#include "mm/vmm.h"
#include "mm/kmalloc.h"
#include "sched/sched.h"
#include "timer/timer.h"
#include "../lib/rand.h"
#include "smp/smp.h"
#include "smp/per_cpu.h"
#include "gdt/tss.h"
#include "syscall/syscall.h"
#include "../user/user.h"
#include "../fs/vfs.h"
#include "../drivers/ata/ata.h"
#include "../fs/mbr/mbr.h"
#include "../fs/ext2/ext2.h"
#include "../tools/nvp/nvp.h"
#include "../tools/nvp/nvpmgr.h"
#include "../tools/nvp/hpt.h"
#include "../net/repo.h"
#include "../net/http.h"
#include "../shell/cli.h"
#include "../shell/shell.h"
#include "../lib/klog.h"
#include <assert.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Limine 请求区标记与基础版本（协议要求，必须存在于 .requests 段）   */
/* ------------------------------------------------------------------ */

__attribute__((used, section(".requests_start_marker")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".requests_end_marker")))
static volatile LIMINE_REQUESTS_END_MARKER;

__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(0);

/* ------------------------------------------------------------------ */
/* Limine 协议请求（必须位于 .requests 段，由 linker.ld 收集）       */
/* ------------------------------------------------------------------ */

/* 引导器信息：打印 Limine 版本（用于验收日志） */
__attribute__((used, section(".requests")))
static volatile struct limine_bootloader_info_request bootloader_info_request = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST,
    .revision = 0,
    .response = NULL,
};

/* GOP 帧缓冲：所有图形输出都基于它 */
__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
    .response = NULL,
};

/* 内存映射：阶段五（物理内存管理器）使用，此处先请求并保存 */
__attribute__((used, section(".requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
    .response = NULL,
};

/* 高半区直接映射：阶段七（分页）使用 */
__attribute__((used, section(".requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0,
    .response = NULL,
};

/* 内核物理/虚拟基址：阶段七分页使用 */
__attribute__((used, section(".requests")))
static volatile struct limine_kernel_address_request kernel_address_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0,
    .response = NULL,
};

/* ------------------------------------------------------------------ */
/* 阶段九/十：测试任务                                              */
/* ------------------------------------------------------------------ */

static void task_a(void)
{
    for (int i = 0; i < 5; i++) {
        terminal_printf("A");
        yield();
    }
}

static void task_b(void)
{
    for (int i = 0; i < 5; i++) {
        terminal_printf("B");
        yield();
    }
}

/* 阶段十：燃烧任务（先错峰 sleep 演示阻塞唤醒，再烧 80ms CPU 计数） */
static volatile uint64_t burn_cnt[3];
static volatile int burner_done[3];

static void burner_0(void)
{
    sleep(10);
    uint64_t end = timer_get_ticks() + 80;
    while (timer_get_ticks() < end)
        burn_cnt[0]++;
    burner_done[0] = 1;
}

static void burner_1(void)
{
    sleep(20);
    uint64_t end = timer_get_ticks() + 80;
    while (timer_get_ticks() < end)
        burn_cnt[1]++;
    burner_done[1] = 1;
}

static void burner_2(void)
{
    sleep(30);
    uint64_t end = timer_get_ticks() + 80;
    while (timer_get_ticks() < end)
        burn_cnt[2]++;
    burner_done[2] = 1;
}

/* 阶段十一：每-CPU 燃烧任务（验证多核调度与 LAPIC 定时器） */
enum { SMP_TEST_N = 4 };
static volatile uint64_t smp_cnt[SMP_TEST_N];
static volatile int smp_done[SMP_TEST_N];
static volatile uint32_t smp_ran_cpu[SMP_TEST_N];

static void smp_burn_cpu(void)
{
    uint32_t me = this_cpu()->cpu_id;
    if (me < SMP_TEST_N) {
        smp_ran_cpu[me] = me;          /* 记录实际运行的 CPU（验证绑定） */
        uint64_t end = timer_get_ticks() + 80;
        while (timer_get_ticks() < end)
            smp_cnt[me]++;
        smp_done[me] = 1;
    }
}

/* ------------------------------------------------------------------ */
/* 内核入口                                                          */
/* ------------------------------------------------------------------ */

static void hcf(void)
{
    for (;;) {
        __asm__ volatile("hlt");
    }
}

/* ==================================================================== */
/* Nova v2：分阶段启动（HBOS 架构模式）
 *   Phase 1: serial + framebuffer + font + terminal
 *   Phase 2: GDT/IDT/TSS/syscall
 *   Phase 3: 内存（pmm/buddy/vmm/kmalloc）
 *   Phase 4: 设备与中断（PIC/keyboard/timer/调度/SMP/用户态）
 *   Phase 5: 文件系统（tmpfs/ata/mbr/ext2/包管理）
 *   Phase 6: 网络（e1000/repo）
 *   Phase 7: 多盘挂载 + 持久化
 *   Phase 8: Shell（命令注册 + 自检 + 欢迎 + CLI）
 * ==================================================================== */

/* 跨 phase 状态（文件级，供 phase6/7/8 共享） */
static int boot_installed;   /* 根目录有 kernel.elf = 已安装系统 */
static char sys_letter;      /* 首个已安装系统盘盘符 */
static int sys_slot;         /* 对应 ext2 槽位 */
static struct limine_framebuffer *g_fb;   /* GOP 帧缓冲（phase1 提供） */

static void phase1_early(void)
{
    uart_init();
    uart_printf("\r\n[Nova] boot: kmain entered\r\n");

    if (bootloader_info_request.response != NULL) {
        uart_printf("[Nova] bootloader: %s %s\r\n",
                    bootloader_info_request.response->name,
                    bootloader_info_request.response->version);
    }

    if (framebuffer_request.response == NULL ||
        framebuffer_request.response->framebuffer_count < 1) {
        uart_printf("[Nova] ERROR: no framebuffer provided by bootloader\r\n");
        hcf();
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    g_fb = fb;

    uart_printf("[Nova] framebuffer: address=%p width=%u height=%u pitch=%u bpp=%u\r\n",
                fb->address, (unsigned)fb->width, (unsigned)fb->height,
                (unsigned)fb->pitch, (unsigned)fb->bpp);

    /* ---- 字体初始化 ---- */
    if (font_init() != 0) {
        uart_printf("[Nova] ERROR: PSF2 font validation failed\r\n");
        hcf();
    }
    uart_printf("[Nova] font: Terminus PSF2 %ux%u\r\n", FONT_WIDTH, FONT_HEIGHT);

    /* ---- 终端初始化 ---- */
    int err = terminal_init((uint32_t *)fb->address, fb->pitch,
                            (uint32_t)fb->width, (uint32_t)fb->height,
                            fb->red_mask_shift, fb->green_mask_shift,
                            fb->blue_mask_shift);
    if (err != 0) {
        uart_printf("[Nova] ERROR: terminal_init failed, errno=%d\r\n", -err);
        hcf();
    }
    uart_puts("[Nova] terminal: 80x32 initialized\r\n");

    /* ---- 彩色日志（阶段二验收标准） ---- */
    terminal_set_color(TERM_COLOR_WHITE, TERM_COLOR_BLACK);
    terminal_printf("Nova v0.1 - ");
    terminal_set_color(TERM_COLOR_BRIGHT_GREEN, TERM_COLOR_BLACK);
    terminal_printf("[OK] ");
    terminal_set_color(TERM_COLOR_LIGHT_GRAY, TERM_COLOR_BLACK);
    terminal_printf("Framebuffer initialized\n");

    terminal_set_color(TERM_COLOR_BRIGHT_CYAN, TERM_COLOR_BLACK);
    terminal_printf("[INFO] ");
    terminal_set_color(TERM_COLOR_LIGHT_GRAY, TERM_COLOR_BLACK);
    terminal_printf("Resolution: %ux%u, pitch=%u, bpp=%u\n",
                    (unsigned)fb->width, (unsigned)fb->height,
                    (unsigned)fb->pitch, (unsigned)fb->bpp);

    terminal_set_color(TERM_COLOR_BRIGHT_CYAN, TERM_COLOR_BLACK);
    terminal_printf("[INFO] ");
    terminal_set_color(TERM_COLOR_LIGHT_GRAY, TERM_COLOR_BLACK);
    terminal_printf("Font: Terminus %ux%u, %u glyphs (PSF2)\n",
                    FONT_WIDTH, FONT_HEIGHT, PSF2FONT_NUMGLYPH);

    terminal_set_color(TERM_COLOR_BRIGHT_GREEN, TERM_COLOR_BLACK);
    terminal_printf("[OK] ");
    terminal_set_color(TERM_COLOR_LIGHT_GRAY, TERM_COLOR_BLACK);
    terminal_printf("Terminal initialized (%ux%u)\n",
                    TERMINAL_COLS, TERMINAL_ROWS);
}

/* Phase 2: GDT/IDT/TSS/每-CPU/syscall */
static void phase2_cpu(void)
{
    /* ---- 阶段三：GDT 与 IDT ---- */
    terminal_set_color(TERM_COLOR_BRIGHT_CYAN, TERM_COLOR_BLACK);
    terminal_printf("[INFO] ");
    terminal_set_color(TERM_COLOR_LIGHT_GRAY, TERM_COLOR_BLACK);
    terminal_printf("Loading GDT...\n");

    if (gdt_init() != 0) {
        terminal_printf("[ERROR] GDT init failed\n");
        hcf();
    }
    uart_printf("[Nova] GDT: loaded (null/kcode/kdata/ucode/udata)\n");

    terminal_printf("Loading IDT...\n");
    if (idt_init() != 0) {
        terminal_printf("[ERROR] IDT init failed\n");
        hcf();
    }
    uart_printf("[Nova] IDT: 256 vectors installed\n");

    terminal_set_color(TERM_COLOR_BRIGHT_GREEN, TERM_COLOR_BLACK);
    terminal_printf("[OK] ");
    terminal_set_color(TERM_COLOR_LIGHT_GRAY, TERM_COLOR_BLACK);
    terminal_printf("GDT + IDT initialized\n");

    /* 阶段十二：加载 TSS（ring3->ring0 内核栈切换） */
    tss_init();

    /* 阶段十一：BSP 每-CPU 槽位 + GS 基址（必须在任何 tick/调度前；
     * 且在 gdt_init 之后，因为 gdt_flush 会重载 GS 选择子） */
    bsp_per_cpu_init();
    uart_printf("[Nova] per-CPU: BSP this_cpu ready (LAPIC ID %u)\r\n",
                (unsigned)this_cpu()->lapic_id);

    /* ---- INT 0x80 软件中断测试（压缩为一行） ---- */
    if (syscall_init() != 0) {
        terminal_printf("[ERROR] syscall_init failed\n");
        hcf();
    }

    uint64_t test_val = 42;
    __asm__ volatile(
        "mov %0, %%rax\n\t"
        "mov $0xdead, %%rbx\n\t"
        "int $0x80"
        : : "r"(test_val) : "rax", "rbx", "memory");
    terminal_printf("INT 0x80 test: rax=42 rbx=0xdead OK\n");
}

/* Phase 3: 内存（pmm/buddy/vmm/kmalloc） */
static void phase3_mem(void)
{
    uint64_t kphys0 = kernel_address_request.response
                          ? kernel_address_request.response->physical_base : 0;
    uint64_t kvirt0 = kernel_address_request.response
                          ? kernel_address_request.response->virtual_base
                          : 0xffffffff80000000ull;
    int perr = pmm_init(memmap_request.response,
                        hhdm_request.response ? hhdm_request.response->offset : 0,
                        kphys0, kvirt0);
    if (perr != 0) {
        terminal_printf("[ERROR] pmm_init failed errno=%d\n", -perr);
        hcf();
    }
    uart_printf("[Nova] Physical Memory Manager initialized\r\n");
    pmm_print_usage();

    /* 自检：分配 3 连续页 + 1 页，释放后空闲页数应回到基线（无泄漏） */
    uint64_t free_before = pmm_free_page_count();
    uint64_t ta = pmm_alloc_pages(3);
    uint64_t tb = pmm_alloc_page();
    pmm_free_pages(ta, 3);
    pmm_free_page(tb);
    terminal_printf("[TEST] pmm: alloc 3pages+1page freed, leak=%s\n",
                    free_before == pmm_free_page_count() ? "no" : "YES!");
    uart_printf("[Nova] PMM self-test: alloc+free, leak=%s\r\n",
                free_before == pmm_free_page_count() ? "no" : "YES");
    klog("[OK] pmm: physical memory manager");

    /* ---- 阶段六：Buddy System 分配器 ---- */
    uint64_t hhdm = hhdm_request.response ? hhdm_request.response->offset : 0;
    if (buddy_init(hhdm) != 0) {
        terminal_printf("[ERROR] buddy_init failed\n");
        hcf();
    }
    uart_printf("[Nova] Buddy allocator initialized\r\n");
    buddy_print_stats();

    /* 自检：100 个随机 order 块，分配后逆序释放，统计应回到基线。
     * 随机 order 限 0-8（4KB-1MB）；若某 order 恰好无块则逐级降阶重试 */
    srand(0x0BAD5EEDu);
    uint64_t buddy_before = buddy_free_pages();
    enum { BUDDY_TEST_N = 100 };
    uint64_t blocks[BUDDY_TEST_N];
    int orders[BUDDY_TEST_N];

    for (int i = 0; i < BUDDY_TEST_N; i++) {
        int o = (int)(rand() % (BUDDY_MAX_ORDER - 1));
        while (o >= 0 && (blocks[i] = buddy_alloc(o)) == 0)
            o--;
        orders[i] = o;
        assert(o >= 0);   /* 池内仍有内存时不应全部失败 */
    }
    for (int i = BUDDY_TEST_N - 1; i >= 0; i--)
        buddy_free(blocks[i], orders[i]);

    terminal_printf("[TEST] buddy: %d random blocks alloc+free, leak=%s\n",
                    BUDDY_TEST_N,
                    buddy_before == buddy_free_pages() ? "no" : "YES!");
    uart_printf("[Nova] Buddy self-test: %d blocks, leak=%s\r\n",
                BUDDY_TEST_N,
                buddy_before == buddy_free_pages() ? "no" : "YES");
    klog("[OK] buddy: order 0-10 allocator");

    /* ---- 阶段七：分页与虚拟内存 ---- */
    uint64_t kphys = kernel_address_request.response
                         ? kernel_address_request.response->physical_base
                         : 0;
    uint64_t kvirt = kernel_address_request.response
                         ? kernel_address_request.response->virtual_base
                         : 0xffffffff80000000ull;
    assert(kphys != 0 && hhdm != 0);

    int verr = vmm_init(kphys, kvirt, hhdm);
    if (verr != 0) {
        terminal_printf("[ERROR] vmm_init failed errno=%d\n", -verr);
        hcf();
    }
    uart_printf("[Nova] Paging enabled: CR3=0x%llx kernel 0x%llx->0x%llx hhdm=0x%llx\r\n",
                (unsigned long long)vmm_get_cr3(),
                (unsigned long long)kvirt, (unsigned long long)kphys,
                (unsigned long long)hhdm);

    /* 虚拟地址验证：
     *   identity：virt_to_phys(0x100000) == 0x100000
     *   内核高半区：virt_to_phys(0xffffffff80001000) == kphys + 0x1000
     *   帧缓冲：phys_to_virt(fb_phys) 应等于终端当前 fb 地址（HHDM 映射）
     *   按需映射：vmm_alloc_page 分配并读写 */
    uint64_t idp = virt_to_phys(0x100000ull);
    uint64_t kernp = virt_to_phys(0xffffffff80001000ull);
    assert(idp == 0x100000ull);
    assert(kernp == kphys + 0x1000);

    uint64_t fb_phys = (uint64_t)(uintptr_t)g_fb->address - hhdm;
    uint64_t fb_virt = phys_to_virt(fb_phys);
    assert(fb_virt == (uint64_t)(uintptr_t)g_fb->address);
    (void)fb_phys;

    uint64_t testv = 0xffffffffc0000000ull;
    uint64_t testp = vmm_alloc_page(testv);
    assert(testp != 0);
    *(volatile uint32_t *)(uintptr_t)testv = 0x12345678u;
    assert(*(volatile uint32_t *)(uintptr_t)testv == 0x12345678u);
    assert(virt_to_phys(testv) == testp);

    terminal_set_color(TERM_COLOR_BRIGHT_GREEN, TERM_COLOR_BLACK);
    terminal_printf("[OK] ");
    terminal_set_color(TERM_COLOR_LIGHT_GRAY, TERM_COLOR_BLACK);
    terminal_printf("Paging: 4-level, CR3=0x%llx\n",
                    (unsigned long long)vmm_get_cr3());

    terminal_printf("[TEST] vmm: ident=0x%llx kern=0x%llx alloc@0x%llx OK\n",
                    (unsigned long long)idp, (unsigned long long)kernp,
                    (unsigned long long)testv);
    uart_printf("[Nova] vmm test: ident ok, kernel map ok, fb=0x%llx, alloc r/w ok\r\n",
                (unsigned long long)fb_virt);
    klog("[OK] paging: 4-level vmm");

    /* ---- 阶段八：内核堆分配器 ---- */
    if (kmalloc_init(hhdm) != 0) {
        terminal_printf("[ERROR] kmalloc_init failed\n");
        hcf();
    }
    uart_printf("[Nova] kmalloc initialized\r\n");
    terminal_printf("[OK] kmalloc: 8 slab caches (32..4096) + large allocs\n");

    /* 自检：100 个随机大小对象（1..8192 字节，覆盖 slab 与大对象路径） */
    srand(0xCAFEBABEu);
    enum { KTEST_N = 100 };
    void *ptrs[KTEST_N];
    size_t sizes[KTEST_N];
    for (int i = 0; i < KTEST_N; i++) {
        sizes[i] = 1 + (size_t)(rand() % 8192);
        ptrs[i] = kmalloc(sizes[i]);
        assert(ptrs[i] != NULL);
        memset(ptrs[i], 0xAB, sizes[i]);   /* 验证对象可写 */
        assert(((uint8_t *)ptrs[i])[sizes[i] - 1] == 0xAB);
    }
    for (int i = 0; i < KTEST_N; i++)
        kfree(ptrs[i]);

    terminal_printf("[TEST] kmalloc: %d random sizes alloc+free, leak=%s, hit=%u%%\n",
                    KTEST_N, kmalloc_active_objects() == 0 ? "no" : "YES!",
                    (unsigned)kmalloc_hit_rate());
    uart_printf("[Nova] kmalloc self-test: %d objects, leak=%s\r\n",
                KTEST_N, kmalloc_active_objects() == 0 ? "no" : "YES");
    klog("[OK] kmalloc: slab heap");
    kmalloc_print_stats();

    /* ---- 阶段二十：CJK 位图字体（HZK16，构建期渲染，取代 FreeType）----
     * scripts/gen-cjk.py 在构建期把文渊 TTF 渲染成 HZKH 位图 blob，
     * 运行时 font_cjk 纯查表 + blit——根治坑 4.1 FreeType 越界写。 */
    {
        int ferr = font_sans_init();
        if (ferr != 0) {
            terminal_printf("[ERROR] font_cjk init failed errno=%d "
                            "(fallback: mono)\n", -ferr);
            uart_printf("[Nova] font_cjk: init failed errno=%d, "
                        "fallback to mono\r\n", ferr);
        } else {
            uart_printf("[Nova] font_cjk: bitmap CJK ready\r\n");
            /* 启动即切换为 CJK 位图字体（含中文显示）；中文欢迎行在
             * shell 自检之后、CLI 提示符前输出（避免被滚动覆盖） */
            int serr = terminal_set_font(TERM_FONT_WENYUAN);
            if (serr == 0) {
                uart_printf("[Nova] font_cjk: CJK bitmap font loaded\r\n");
            } else {
                uart_printf("[Nova] font_cjk: terminal_set_font errno=%d\r\n",
                            serr);
            }
        }
    }
}

/* Phase 4: 设备与中断（PIC/keyboard/timer/调度/SMP/用户态） */
static void phase4_devices(void)
{
    /* ---- 阶段四：PIC + 键盘 ---- */
    terminal_set_color(TERM_COLOR_BRIGHT_CYAN, TERM_COLOR_BLACK);
    terminal_printf("[INFO] ");
    terminal_set_color(TERM_COLOR_LIGHT_GRAY, TERM_COLOR_BLACK);
    terminal_printf("Initializing PIC...\n");

    pic_init();   /* 重映射 IRQ0-15 -> 0x20-0x2F，屏蔽全部 */
    uart_printf("[Nova] PIC: remapped IRQ0-15 to vectors 0x20-0x2F, all masked\r\n");

    if (keyboard_init() != 0) {
        terminal_printf("[ERROR] keyboard_init failed\n");
        hcf();
    }
    uart_printf("[Nova] keyboard: IRQ1 unmasked, handler installed\r\n");

    /* 系统时钟（1000Hz）：驱动光标闪烁，阶段十用于调度 */
    if (timer_init() != 0) {
        terminal_printf("[ERROR] timer_init failed\n");
        hcf();
    }
    uart_printf("[Nova] timer: PIT channel 0 at 1000Hz, IRQ0 armed\r\n");

    terminal_set_color(TERM_COLOR_BRIGHT_GREEN, TERM_COLOR_BLACK);
    terminal_printf("[OK] ");
    terminal_set_color(TERM_COLOR_LIGHT_GRAY, TERM_COLOR_BLACK);
    terminal_printf("Keyboard + timer ready - type to echo (Ctrl+Alt+Del = reboot)\n");

    /* ---- 开启中断 ---- */
    sti();
    uart_printf("[Nova] Phase4 OK: interrupts enabled\r\n");

    /* ---- 阶段九：任务管理与轮转调度 ---- */
    if (task_init() != 0) {
        terminal_printf("[ERROR] task_init failed\n");
        hcf();
    }
    create_task(task_a);
    create_task(task_b);

    terminal_printf("[OK] Tasks A/B: round-robin ");
    yield();   /* 让出 CPU：两任务轮转打印 ABABABABAB */
    terminal_printf(" OK\n");
    uart_printf("[Nova] scheduler: round-robin done\r\n");

    /* ---- 阶段十：抢占式调度（10ms 时间片 + sleep） ---- */
    create_task(burner_0);
    create_task(burner_1);
    create_task(burner_2);
    terminal_printf("[OK] Preemptive: 10ms slices, 3 burners (sleep 10/20/30ms)\n");
    yield();   /* 让燃烧任务开始（先进入 sleep） */
    while (!(burner_done[0] && burner_done[1] && burner_done[2]))
        yield();   /* 等全部燃烧任务完成（睡眠唤醒 + 抢占调度） */

    terminal_printf("[TEST] fairness: a=%llu b=%llu c=%llu\n",
                    (unsigned long long)burn_cnt[0],
                    (unsigned long long)burn_cnt[1],
                    (unsigned long long)burn_cnt[2]);
    uart_printf("[Nova] fairness: a=%llu b=%llu c=%llu\r\n",
                (unsigned long long)burn_cnt[0],
                (unsigned long long)burn_cnt[1],
                (unsigned long long)burn_cnt[2]);

    /* ---- 阶段十一：SMP 多核启动 ---- */
    int serr = smp_init();
    if (serr != 0) {
        terminal_printf("[ERROR] smp_init failed errno=%d\n", -serr);
        hcf();
    }
    uint32_t online = smp_wait_online();
    uart_printf("[Nova] SMP: %u/%u CPUs online\r\n", online, cpu_count);
    klogf("[OK] smp: %u cpus online", (unsigned)online);

    /* 每-CPU 燃烧任务：验证各 CPU 独立调度 + LAPIC 定时器 */
    uint32_t nt = cpu_count;
    if (nt > SMP_TEST_N)
        nt = SMP_TEST_N;
    for (uint32_t i = 0; i < nt; i++)
        create_task_on(i, smp_burn_cpu);

    uint64_t smp_deadline = timer_get_ticks() + 5000;   /* 5 秒超时 */
    for (;;) {
        bool all_done = true;
        for (uint32_t i = 0; i < nt; i++) {
            if (!smp_done[i])
                all_done = false;
        }
        if (all_done || timer_get_ticks() > smp_deadline)
            break;
        yield();
    }

    bool pin_ok = true;
    for (uint32_t i = 0; i < nt; i++) {
        if (smp_ran_cpu[i] != i)
            pin_ok = false;
    }
    /* 按实际 CPU 数显示计数（1 CPU 时只显示 c0，避免误导性的零） */
    terminal_printf("[TEST] smp: ");
    for (uint32_t i = 0; i < nt; i++)
        terminal_printf("c%u=%llu ", (unsigned)i,
                        (unsigned long long)smp_cnt[i]);
    terminal_printf("(%u CPUs)\n", (unsigned)cpu_count);
    uart_printf("[Nova] smp test: ");
    for (uint32_t i = 0; i < nt; i++)
        uart_printf("c%u=%llu ", (unsigned)i,
                    (unsigned long long)smp_cnt[i]);
    uart_printf("pin=%s (%u CPUs)\r\n",
                pin_ok ? "OK" : "FAIL", (unsigned)cpu_count);

    /* ---- 阶段十二：用户态与系统调用 ---- */
    if (user_init() != 0) {
        terminal_printf("[ERROR] user_init failed\n");
        hcf();
    }
    uart_printf("[Nova] user: creating ring3 task\r\n");
    struct task *ut = create_user_task(NOVA_USER_CODE_BASE,
                                       NOVA_USER_STACK_TOP);
    assert(ut != NULL);
    yield();   /* 运行用户任务（ring3）直至其 sys_exit 退出 */

    /* 用户程序经 sys_write(fd=1) 在屏幕打印一行（ring3 证据）；
     * 此处仅输出串口，避免增加屏幕行破坏 25 行布局。 */
    uart_printf("[Nova] user mode demo done (task %llu)\r\n",
                (unsigned long long)ut->pid);
    klog("[OK] user: ring3 syscalls");
}

/* Phase 5: 文件系统（tmpfs/ata/mbr/ext2/包管理） */
static void phase5_fs(void)
{
    /* ---- 阶段十三：tmpfs 内存文件系统 ---- */
    {
        char tb[512];
        size_t got;
        int err;

        err = vfs_init();
        if (err != 0) {
            terminal_printf("[ERROR] vfs_init errno=%d\n", -err);
            hcf();
        }

        /* 1. 创建 /readme.txt 并写入 */
        err = vfs_create("/readme.txt", VFS_TYPE_FILE, NULL);
        assert(err == 0);
        struct file *rf;
        err = vfs_open("/readme.txt", &rf);
        assert(err == 0);
        err = vfs_write(rf, "Hello Nova tmpfs!\n", 18);
        assert(err == 0);
        vfs_close(rf);

        /* 2. 读回校验 */
        err = vfs_open("/readme.txt", &rf);
        assert(err == 0);
        err = vfs_read(rf, tb, sizeof(tb), &got);
        assert(err == 0 && got == 18);
        assert(memcmp(tb, "Hello Nova tmpfs!\n", 18) == 0);
        vfs_close(rf);

        /* 3. 目录 + 子文件 */
        err = vfs_create("/docs", VFS_TYPE_DIR, NULL);
        assert(err == 0);
        err = vfs_create("/docs/notes.txt", VFS_TYPE_FILE, NULL);
        assert(err == 0);

        /* 4. 1KB 数据完整性：0..255 模式循环 */
        {
            uint8_t pat[1024], rd[1024];
            for (int i = 0; i < 1024; i++)
                pat[i] = (uint8_t)i;
            err = vfs_create("/data.bin", VFS_TYPE_FILE, NULL);
            assert(err == 0);
            err = vfs_open("/data.bin", &rf);
            assert(err == 0);
            err = vfs_write(rf, pat, sizeof(pat));
            assert(err == 0);
            vfs_close(rf);
            err = vfs_open("/data.bin", &rf);
            assert(err == 0);
            err = vfs_read(rf, rd, sizeof(rd), &got);
            assert(err == 0 && got == sizeof(rd));
            assert(memcmp(pat, rd, sizeof(pat)) == 0);
            vfs_close(rf);
        }

        /* 5. 列表 */
        err = vfs_list("/", tb, sizeof(tb));
        assert(err == 0);
        uart_printf("[Nova] tmpfs: /\n%s", tb);

        /* 6. 删除 + 确认 */
        err = vfs_unlink("/readme.txt");
        assert(err == 0);
        {
            struct file *gone;
            err = vfs_open("/readme.txt", &gone);
            assert(err == -ENOENT);
        }

        terminal_printf("[OK] tmpfs: rw+dir+1KB pattern verified\n");
        uart_printf("[Nova] tmpfs test: create/write/read/dir/list/unlink OK\r\n");
        klog("[OK] tmpfs: ram filesystem");
    }

    /* ---- 阶段十四：ATA 硬盘驱动 ---- */
    {
        int aerr = ata_init();
        if (aerr != 0) {
            terminal_printf("[ERROR] ATA: no device (errno=%d)\n", -aerr);
        } else {
            /* 读写测试：写扇区 2 一个模式，读回逐字校验；
             * 扇区 0/1 只读不写（MBR 区域） */
            uint16_t pat[256], rd[256];   /* 一扇区 = 256 字 = 512 字节 */
            for (int i = 0; i < 256; i++)
                pat[i] = (uint16_t)(0x5A00u | (i & 0xFF));
            int rerr = ata_write_sectors(2, 1, pat);
            int serr = rerr == 0 ? ata_read_sectors(2, 1, rd) : rerr;
            bool rw_ok = (rerr == 0 && serr == 0 &&
                          memcmp(pat, rd, sizeof(pat)) == 0);

            if (rw_ok) {
                terminal_printf("[OK] ATA: %s, %lluMB, sector rw OK\n",
                                ata_model(),
                                (unsigned long long)(ata_sector_count() / 2048));
            } else {
                terminal_printf("[ERROR] ATA: sector rw failed (w=%d r=%d)\n",
                                rerr, serr);
            }
            uart_printf("[Nova] ATA test: %s sectors=%llu rw=%s (w=%d r=%d)\r\n",
                        ata_model(),
                        (unsigned long long)ata_sector_count(),
                        rw_ok ? "OK" : "FAIL", rerr, serr);
        }
    }

    /* ---- 阶段十五：MBR 分区解析 ---- */
    {
        struct mbr mbr;
        int merr = mbr_read(&mbr);
        if (merr != 0) {
            terminal_printf("[ERROR] MBR: read failed errno=%d\n", -merr);
        } else if (!mbr_validate(&mbr)) {
            /* 测试盘无分区表：写入示例 MBR 后再解析 */
            uart_printf("[Nova] MBR: no signature, writing sample MBR\r\n");
            mbr_build_sample(&mbr);
            int werr = ata_write_sectors(0, 1, &mbr);
            if (werr != 0) {
                terminal_printf("[ERROR] MBR: write failed errno=%d\n", -werr);
            } else {
                merr = mbr_read(&mbr);
            }
        }

        if (merr == 0 && mbr_validate(&mbr)) {
            int nparts = mbr_count_parts(&mbr);
            terminal_printf("[OK] MBR: 0x55AA, %d primary parts\n", nparts);
            uart_printf("[Nova] MBR test: sig=0x%04x parts=%d\r\n",
                        (unsigned)mbr.signature, nparts);
            klogf("[OK] mbr: %d partitions", nparts);
            for (int i = 0; i < 4; i++) {
                if (mbr.parts[i].type == 0)
                    continue;
                uart_printf("  part%d: type=0x%02x start=%u sectors=%u (%uMB)\r\n",
                            i, (unsigned)mbr.parts[i].type,
                            (unsigned)mbr.parts[i].lba_start,
                            (unsigned)mbr.parts[i].sectors,
                            (unsigned)(mbr.parts[i].sectors / 2048));
            }
        } else {
            terminal_printf("[ERROR] MBR: invalid (sig read failed)\n");
        }
    }

    /* ---- 阶段十六：ext2 只读文件系统 ----
     * 遍历 MBR 全部分区找 0x83（双分区布局下 0x83 在 parts[1]，
     * parts[0] 是 0xEF ESP）。boot_installed：根目录有 kernel.elf 即
     * 已安装系统（后续跳过网络演示测试，加快引导）。文件级变量。 */
    {
        struct mbr mbr;
        bool have_ext2 = false;
        if (mbr_read(&mbr) == 0 && mbr_validate(&mbr)) {
            for (int i = 0; i < 4; i++) {
                if (mbr.parts[i].type != 0x83)
                    continue;
                uint32_t pstart = mbr.parts[i].lba_start;
                uart_printf("[Nova] ext2: mounting part%d @ LBA %u\r\n",
                            i, (unsigned)pstart);
                if (ext2_mount(pstart) == 0) {
                    have_ext2 = true;
                    break;
                }
            }
        }

        if (!have_ext2) {
            terminal_printf("[INFO] ext2: no 0x83 partition with ext2\n");
            uart_printf("[Nova] ext2: not found (no filesystem)\r\n");
        } else {
            struct ext2_inode root;
            int eerr = ext2_read_inode(EXT2_ROOT_INO, &root);
            if (eerr != 0) {
                terminal_printf("[ERROR] ext2: root inode read errno=%d\n",
                                -eerr);
            } else {
                /* 枚举根目录 */
                char name[64];
                uint32_t type, ino;
                uint32_t nfiles = 0;
                uint64_t idx = 0;
                uart_printf("[Nova] ext2: root listing\r\n");
                for (;;) {
                    int r = ext2_read_dir(&root, idx, name, &type, &ino);
                    if (r != 0)
                        break;
                    uart_printf("  %s (%s, ino=%u)\r\n", name,
                                type == 2 ? "dir" : "file", (unsigned)ino);
                    if (type != 2 && strcmp(name, ".") != 0 &&
                        strcmp(name, "..") != 0)
                        nfiles++;
                    if (idx > 64)   /* 安全上限 */
                        break;
                    idx++;
                }

                /* 读取 hello.txt 并验证 */
                uint32_t hino = ext2_lookup(&root, "hello.txt");
                if (hino != 0) {
                    struct ext2_inode hf;
                    char content[128];
                    uint64_t got = 0;
                    if (ext2_read_inode(hino, &hf) == 0 &&
                        ext2_read_file(&hf, 0, content, sizeof(content) - 1,
                                       &got) == 0) {
                        content[got] = '\0';
                        /* 去掉内容中的换行，避免屏幕行折行 */
                        for (char *p = content; *p; p++) {
                            if (*p == '\n' || *p == '\r')
                                *p = ' ';
                        }
                        bool ok = (got >= 15 &&
                                   memcmp(content, "Hello from Nova", 15) == 0);
                        terminal_printf("[OK] ext2: 1KB blk, hello.txt='%s' %s\n",
                                        content,
                                        ok ? "[verified]" : "[MISMATCH]");
                        uart_printf("[Nova] ext2 test: hello.txt content='%s' got=%llu %s\r\n",
                                    content, (unsigned long long)got,
                                    ok ? "OK" : "FAIL");
                        klog("[OK] ext2: read-only fs");
                    } else {
                        terminal_printf("[ERROR] ext2: hello.txt read failed\n");
                    }
                } else {
                    /* 安装盘没有 hello.txt（测试产物）；根目录有 kernel.elf
                     * 即为已安装系统，跳过内容校验（非错误） */
                    uint32_t kino = ext2_lookup(&root, "kernel.elf");
                    if (kino != 0) {
                        boot_installed = 1;
                        terminal_printf("[INFO] ext2: installed system "
                                        "(kernel.elf, hello.txt test "
                                        "skipped)\n");
                    } else
                        terminal_printf("[ERROR] ext2: hello.txt not found\n");
                }
            }
        }
    }

    /* ---- 阶段十七：.nvp 包格式 ---- */
    {
        struct mbr mbr;
        bool ok = false;
        uint32_t nvp_part = 0;
        int nvp_mounted = 0;      /* nvp 分区是否成功挂载（cur 有效） */
        if (mbr_read(&mbr) == 0 && mbr_validate(&mbr)) {
            for (int i = 0; i < 4; i++) {
                if (mbr.parts[i].type == 0x83) {
                    nvp_part = mbr.parts[i].lba_start;
                    break;
                }
            }
        }
        if (nvp_part != 0 && ext2_mount(nvp_part) == 0) {
            nvp_mounted = 1;
            struct ext2_inode root;
            if (ext2_read_inode(EXT2_ROOT_INO, &root) == 0) {
                uint32_t ino = ext2_lookup(&root, "demo.nvp");
                if (ino != 0) {
                    struct ext2_inode f;
                    if (ext2_read_inode(ino, &f) == 0 && f.size <= (1u << 20)) {
                        uint8_t *pkg_buf = kmalloc((size_t)f.size);
                        uint64_t got = 0;
                        if (pkg_buf != NULL &&
                            ext2_read_file(&f, 0, pkg_buf, f.size, &got) == 0 &&
                            got == f.size) {
                            struct nvp_pkg pkg;
                            if (nvp_parse(pkg_buf, (uint32_t)f.size, &pkg) == 0) {
                                uint32_t n = nvp_file_count(&pkg);
                                uart_printf("[Nova] nvp: %u files\r\n",
                                            (unsigned)n);
                                for (uint32_t i = 0; i < n; i++)
                                    uart_printf("  %s\r\n", nvp_name(&pkg, i));

                                const void *hd;
                                uint32_t hsz;
                                if (nvp_read(&pkg, "hello.txt", &hd, &hsz) == 0) {
                                    char buf[64];
                                    uint32_t cl = hsz < sizeof(buf) - 1
                                                      ? hsz
                                                      : sizeof(buf) - 1;
                                    memcpy(buf, hd, cl);
                                    buf[cl] = '\0';
                                    for (char *p = buf; *p; p++) {
                                        if (*p == '\n' || *p == '\r')
                                            *p = ' ';
                                    }
                                    bool vok = (hsz >= 15 &&
                                                memcmp(hd, "Hello from .nvp!",
                                                       15) == 0);
                                    uart_printf("[Nova] nvp: hello.txt='%s' crc=%s\r\n",
                                                buf, vok ? "OK" : "FAIL");
                                    klog("[OK] nvp: package format");
                                    terminal_printf("[OK] nvp: %u files, hello.txt='%s' %s\n",
                                                    (unsigned)n, buf,
                                                    vok ? "[crc ok]" : "[FAIL]");
                                    ok = vok;
                                }
                                kfree(pkg_buf);
                            }
                        }
                    }
                }
            }
        }
        if (!ok) {
            /* 安装盘没有 demo.nvp（测试包），属正常；测试盘才报错。
             * 注意：只有 nvp 分区挂载成功（cur 有效）才能读 ext2——
             * 无 0x83 分区时（如刚清空的盘）调用 ext2_read_inode 会
             * 在 NULL cur 上除零 panic。 */
            uint32_t kino = 0;
            if (nvp_mounted) {
                struct ext2_inode root2;
                if (ext2_read_inode(EXT2_ROOT_INO, &root2) == 0)
                    kino = ext2_lookup(&root2, "kernel.elf");
            }
            if (kino != 0)
                terminal_printf("[INFO] nvp: demo.nvp absent (installed "
                                "disk, skipped)\n");
            else
                terminal_printf("[ERROR] nvp: parse failed\n");
        }
    }

    /* ---- 阶段十八：nvp 包管理 + CLI ---- */
    {
        nvpmgr_init();
        int ierr = nvpmgr_install("demo");   /* 从 ext2 安装 demo.nvp */
        if (ierr == 0) {
            nvpmgr_list();                   /* 串口列出 */
            uint32_t np = nvpmgr_installed();
            terminal_printf("[OK] nvp-cli: %u pkg installed, list OK\n",
                            (unsigned)np);
            uart_printf("[Nova] nvp-cli test: install/list OK (%u pkg)\r\n",
                        (unsigned)np);
            klogf("[OK] nvpmgr: %u pkg installed", (unsigned)np);
        } else if (ierr == -ENOENT) {
            terminal_printf("[INFO] nvp-cli: demo.nvp absent (installed "
                            "disk, skipped)\n");
        } else {
            terminal_printf("[ERROR] nvp-cli: install failed errno=%d\n",
                            -ierr);
        }
    }

    /* ---- Nova v2：HPT 嵌入式仓库（HBOS 架构模式）----
     * 构建期把 build/repo 下的 .nvp 包打包进内核（sysimg_hpt），启动时
     * 解出到 VFS /packages（Packages 清单 + pool 下的 .hax），
     * app 命令据此 list/install/remove。 */
    {
        if (hpt_init)
            hpt_init();              /* core 中为 NULL（见 hpt.h 注释） */
        uint32_t np = hpt_count();
        if (np > 0) {
            char hbuf[192];
            hpt_list_formatted(hbuf, sizeof(hbuf));
            terminal_printf("[OK] hpt-repo: %u package(s) embedded\n%s",
                            (unsigned)np, hbuf);
            uart_printf("[Nova] hpt-repo: %u package(s) embedded\r\n",
                        (unsigned)np);
            klogf("[OK] hpt-repo: %u package(s)", (unsigned)np);
        } else {
            terminal_printf("[WARN] hpt-repo: empty (no build/repo/*.nvp)\n");
        }
    }
}

/* Phase 6: 网络（e1000 + HTTP 软件仓库） */
static void phase6_net(void)
{
    /* ---- 阶段十九：网络栈（e1000）+ HTTP 软件仓库 ----
     * 已安装系统跳过开机演示测试（无 repo 服务器时 SYN 重传会拖慢引导
     * 数分钟）；但网络栈必须始终初始化——否则 shell 的 repo/calc
     * 命令无网可用（e1000 未初始化，connect 报 EHOSTUNREACH）。 */
    {
        int nerr = net_init();      /* e1000 初始化（PCI 8086:100E） */
        if (nerr != 0) {
            terminal_printf("[ERROR] net: e1000 init failed errno=%d\n",
                            -nerr);
            uart_printf("[Nova] net: e1000 init failed errno=%d\r\n", nerr);
        } else if (boot_installed) {
            repo_init();
            terminal_printf("[INFO] net: e1000 up (boot repo test skipped)\n");
            uart_printf("[Nova] net-repo: skipped on installed system\r\n");
        } else {
            repo_init();
            terminal_set_color(TERM_COLOR_BRIGHT_CYAN, TERM_COLOR_BLACK);
            terminal_printf("[INFO] ");
            terminal_set_color(TERM_COLOR_LIGHT_GRAY, TERM_COLOR_BLACK);
            terminal_printf("net: e1000 up, fetching repo index (HTTP)...\n");
            int uerr = -EIO;    /* 哨兵值：非 0 触发首次尝试 */
            /* 网络首连可能因慢速模拟/服务器启动时序失败，重试最多 3 次 */
            for (int attempt = 0; attempt < 3 && uerr != 0; attempt++) {
                if (attempt > 0) {
                    uart_printf("[Nova] net-repo: retry %d\r\n", attempt);
                    sleep(300);
                }
                uerr = repo_update();       /* HTTP GET /index.nvp */
            }
            if (uerr != 0) {
                terminal_printf("[ERROR] net-repo: update failed errno=%d "
                                "(stage: %s)\n",
                                -uerr, http_last_stage());
                uart_printf("[Nova] net-repo: update failed errno=%d "
                            "stage=%s\r\n", uerr, http_last_stage());
            } else {
                uint32_t nr = repo_list();  /* 串口清单 */
                char rbuf[256];
                repo_list_formatted(rbuf, sizeof(rbuf));
                terminal_printf("[OK] net-repo: %u pkgs listed\n%s",
                                (unsigned)nr, rbuf);

                /* 从仓库安装 ext2 中不存在的包（webdemo）；网络偶发失败
                 * 时重试最多 3 次 */
                int ierr = -EIO;
                for (int attempt = 0; attempt < 3 && ierr != 0; attempt++) {
                    if (attempt > 0) {
                        uart_printf("[Nova] net-repo: install retry %d\r\n",
                                    attempt);
                        sleep(300);
                    }
                    ierr = repo_install("webdemo");
                }
                if (ierr == 0) {
                    uint32_t np = nvpmgr_installed();
                    terminal_printf("[OK] net-repo: '%s' installed via HTTP "
                                    "(total %u pkgs)\n",
                                    "webdemo", (unsigned)np);
                    uart_printf("[Nova] net-repo test: repo=%u pkgs, "
                                "HTTP install OK, installed=%u\r\n",
                                (unsigned)nr, (unsigned)np);
                    klog("[OK] net-repo: HTTP repo install");
                } else {
                    terminal_printf("[ERROR] net-repo: install failed "
                                    "errno=%d\n", -ierr);
                    uart_printf("[Nova] net-repo: install failed errno=%d\r\n",
                                ierr);
                }
            }
        }
    }
}

/* Phase 7: 多盘挂载 + 持久化工作盘 */
static void phase7_mount(void)
{
    /* ---- 阶段二十：多盘挂载（C: tmpfs + D:/E:... 物理盘 ext2） ---- */
    {
        int nd = ata_max_devices();   /* 槽位总数（含空槽） */
        char letter = 'D';
        int mounted = 0;
        for (int dev = 0; dev < nd; dev++) {
            if (letter > 'Z')
                break;
            if (ata_select_device(dev) != 0)
                continue;
            struct mbr mbr;
            if (mbr_read(&mbr) != 0 || !mbr_validate(&mbr))
                continue;
            int pstart = -1;
            for (int p = 0; p < 4; p++) {
                if (mbr.parts[p].type == 0x83) {
                    pstart = (int)mbr.parts[p].lba_start;
                    break;
                }
            }
            if (pstart < 0)
                continue;
            int slot = mounted + 1;      /* 槽位 0 被阶段十七占用 */
            if (ext2_mount_slot(slot, (uint32_t)pstart, dev) != 0)
                continue;
            struct inode *r = ext2fs_mount_root(slot);
            if (r == NULL)
                continue;
            struct ext2_fs *fs = ext2_fs_get(slot);
            uint64_t total = fs ? fs->total_bytes : 0;
            uint64_t free = fs ? fs->free_bytes : 0;
            /* 已安装系统盘（根目录含 kernel.elf）标记为 system，
             * 供后续切换为持久化工作盘 */
            int installed = 0;
            if (ext2_select(slot) == 0) {
                struct ext2_inode eroot;
                if (ext2_read_inode(EXT2_ROOT_INO, &eroot) == 0 &&
                    ext2_lookup(&eroot, "kernel.elf") != 0)
                    installed = 1;
            }
            vfs_mount_drive(letter, installed ? "system" : "ext2",
                            r, total, free);
            uart_printf("[Nova] multi-disk: %c: mounted (ext2 slot=%d "
                        "dev%d total=%lluMB free=%lluMB%s)\r\n",
                        letter, slot, dev,
                        (unsigned long long)(total / 1048576),
                        (unsigned long long)(free / 1048576),
                        installed ? " system" : "");
            if (installed && sys_letter == 0) {
                /* 首个已安装盘 = 引导盘（枚举从 dev0 起；GUI 布局下
                 * 引导盘在 ide0.0）——多盘都装系统时不能取最后一个 */
                sys_letter = letter;
                sys_slot = slot;
            }
            letter++;
            mounted++;
        }
        if (mounted > 0) {
            terminal_printf("[OK] multi-disk: %d 物理盘挂载 "
                            "(C:系统 D:...)\n", mounted);
            uart_printf("[Nova] multi-disk: %d drive(s) mounted\r\n", mounted);
        }
    }
}

/* Phase 8: Shell（命令注册 + 自检 + 持久化 + 欢迎） */
static void phase8_shell(void)
{
    /* ---- 阶段二十：Shell 自检（fs/redirect/pipe/env/builtins） ----
     * Nova v2：先注册命令表（command registry），再跑自检。 */
    shell_commands_init();
    shell_selftest();

    /* ---- 持久化：若引导了已安装系统盘，把 shell 工作盘切到它上面。
     * 放在自检之后：自检路径无盘符前缀，仍落在 C: tmpfs，语义不变；
     * 切盘后 shell 的 "/" 即系统盘根，文件创建/删除真正落盘。 ---- */
    if (sys_letter != 0 && sys_slot >= 0) {
        if (ext2_select(sys_slot) == 0) {
            struct ext2_inode eroot;
            if (ext2_read_inode(EXT2_ROOT_INO, &eroot) == 0) {
                if (ext2_lookup(&eroot, "dict.dat") != 0)
                    terminal_printf("[INFO] dict.dat: present on disk "
                                    "(IME dictionary source)\n");
                else
                    terminal_printf("[INFO] dict.dat: absent on disk\n");
            }
        }
        vfs_set_current_drive(sys_letter);
        terminal_printf("[OK] persistence: work drive %c: (installed "
                        "system ext2 - files survive reboot)\n",
                        sys_letter);
        uart_printf("[Nova] persistence: current drive %c: (installed "
                    "system)\r\n", sys_letter);

        /* 包管理器持久目录：软件包装到硬盘（重启不丢） */
        char pkgpath[16];
        pkgpath[0] = sys_letter;
        pkgpath[1] = ':';
        strcpy(pkgpath + 2, "/pkgs");
        nvpmgr_set_dir(pkgpath);
    }

    /* ---- CJK 位图字体欢迎行（放自检后，启动画面可见） ---- */
    if (font_sans_ready()) {
        uart_printf("[Nova] welcome: printing CJK lines\r\n");
        terminal_set_color(TERM_COLOR_BRIGHT_GREEN, TERM_COLOR_BLACK);
        terminal_printf("[OK] font_cjk: 中文位图字体 (HZK16, 构建期渲染) "
                        "loaded\n");
        terminal_printf("你好，Nova！位图字体渲染中文成功。\n");
        terminal_set_color(TERM_COLOR_LIGHT_GRAY, TERM_COLOR_BLACK);
        terminal_printf("可用命令: font list / font set mono|wenyuan "
                        "切换字体\n");
        uart_printf("[Nova] welcome: done\r\n");
    }
}

/* ==================================================================== */
/* 入口：kmain —— 分阶段启动（HBOS 架构模式）                           */
/* ==================================================================== */
void kmain(void)
{
    phase1_early();     /* serial + framebuffer + font + terminal */
    phase2_cpu();       /* GDT/IDT/TSS/syscall */
    phase3_mem();       /* pmm/buddy/vmm/kmalloc */
    phase4_devices();   /* PIC/keyboard/timer/调度/SMP/用户态 */
    phase5_fs();        /* tmpfs/ata/mbr/ext2/包管理 */
    phase6_net();       /* e1000 + HTTP 仓库 */
    phase7_mount();     /* 多盘挂载 */
    phase8_shell();     /* 命令注册 + 自检 + 持久化 + 欢迎 */

    /* ---- 命令行主循环（光标闪烁 + CLI 输入） ---- */
    uart_printf("[Nova] cli: start\r\n");
    cli_start();
    uart_printf("[Nova] cli: started\r\n");
    for (;;) {
        keyboard_service();                /* 状态键变化时同步 LED */
        terminal_cursor_tick(timer_get_ticks());  /* 光标闪烁 */
        int c = keyboard_getchar();
        if (c >= 0)
            cli_feed_char((char)c);
        hlt();   /* 等待中断唤醒（PIT 1000Hz + 键盘 IRQ1） */
    }
}
