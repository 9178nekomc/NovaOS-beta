# Nova OS

x86_64 原创操作系统内核（非 Linux 发行版）。
架构：x86_64 | 引导：UEFI + Limine v8.x | 图形：GOP 帧缓冲（1920x1080x32bpp）| 语言：C + GAS

## 目录结构

```
boot/     - 启动汇编（boot.S：64 位入口、64KB 栈、SSE 启用、跳转 kmain）
kernel/   - 内核 C 源码（kernel.c + 各子系统目录）
build/    - 构建产物（kernel.elf、iso_root/、nova.iso、测试日志）
scripts/  - 工具链安装 / Limine 克隆构建 / QEMU 自动验收脚本
docs/     - 架构 / 构建 / 测试文档（阶段二十）
.github/  - 持续集成 CI（阶段二十）
limine/   - Limine v8.7.0 引导器（克隆 + 构建产物）
```

## 构建与运行

```bash
# Ubuntu/Debian 或 WSL：
bash scripts/setup-toolchain.sh   # 安装 gcc/make/nasm/xorriso/mtools/qemu/ovmf 等
bash scripts/clone-limine.sh      # 克隆并构建 Limine v8.x（需 autoconf）

make iso                          # 构建 build/nova.iso
make run-uefi                     # QEMU UEFI 运行（需 OVMF）
make run-bios                     # QEMU BIOS 运行
bash scripts/test-qemu.sh         # 自动验收：串口日志 + 截图像素校验
```

Windows + WSL 注意事项：
- Limine 源码构建在 WSL 内进行（见 scripts/limine-prep-wsl.sh、limine-build-wsl.sh），
  产物复制回 limine/；drvfs 上构建极慢。
- Windows 本地代理仅监听 127.0.0.1 时 WSL 不可达，WSL 直连 GitHub 通常可用。
- `make clean` 会删除 build/（含 OVMF 固件）；`scripts/run-qemu.ps1` 在
  build\ovmf.fd 缺失时会自动从 WSL（/usr/share/ovmf/OVMF.fd 等）复制，无需手动处理。
- 图形验收用 Windows 原生 QEMU（D:\qemu）+ `scripts/run-qemu.ps1`；
  WSL 内无 WSLg（DISPLAY 为空），不适合开图形窗口，仅用于构建与无头测试。

## 进度

| 阶段 | 内容 | 状态 |
|------|------|------|
| 1 | 开发环境与最小内核 | ✅ 已验收 |
| 2 | PSF2 字体渲染与终端 | ✅ 已验收 |
| 3 | GDT 与 IDT | ✅ 已验收 |
| 4 | 键盘中断与 PS/2 驱动 | ✅ 已验收 |
| 5 | 物理内存探测与位图分配器 | ✅ 已验收 |
| 6 | Buddy System 分配器 | ✅ 已验收 |
| 7 | 分页与虚拟内存 | ✅ 已验收 |
| 8 | 内核堆分配器 kmalloc/kfree | ✅ 已验收 |
| 9 | 任务管理与上下文切换（单核） | ✅ 已实现并验证（本文件记录） |
| 10 | 抢占式调度（时间片 + sleep） | ✅ 已验收 |
| 11 | SMP 多核启动与每-CPU 调度 | ✅ 已验收 |
| 12 | 用户态（ring3）与系统调用 | ✅ 已验收 |
| 13 | VFS 与 tmpfs 内存文件系统 | ✅ 已验收 |
| 14 | ATA PIO 硬盘驱动 | ✅ 已验收 |
| 15 | MBR 分区解析 | ✅ 已验收 |
| 16 | ext2 只读文件系统 | ✅ 已验收 |
| 17 | NVP 包格式（归档 + CRC） | ✅ 已验收 |
| 18 | 包管理器 + nova$ CLI | ✅ 已验收 |
| 19 | 网络栈（e1000+ARP+TCP+HTTP）+ 软件仓库 | ✅ 已验收 |
| 20 | Shell（40+ 命令/管道/重定向/变量）+ 文档 + CI | ✅ 已验收 |
| 21 | 文渊黑体（FreeType）+ 中文显示 + `font` 字体切换 | ✅ 已实现 |
| 22 | 安装程序：选盘安装 → 硬盘直启（UEFI/GOP + BIOS/VBE 双启动） | ✅ 已实现 |
| 23 | 可写 ext2 + 持久化（create/mkdir/rm/rmdir/write + 硬盘工作盘） | ✅ 已验收 |
| 24 | 软件包交付：`calc` 计算器命令经 HTTP 仓库拉取安装（ops.txt 驱动） | ✅ 已验收 |

### 安装程序与硬盘直启（阶段二十二）

内核同时作为"安装程序"：把系统安装到目标硬盘后，该硬盘可脱离 ISO 直接启动，
**同时支持 UEFI（GOP）与 BIOS（VBE）两条启动路径**。

```bash
bash scripts/test-install.sh    # 端到端验收：ISO 安装 → UEFI+BIOS 双路径直启 → Nova shell
```

安装流程（在 Nova shell 里执行 `install`）：
1. 选择目标盘（枚举 ATA 设备，`install <dev序号> [yes]` 可自动安装）；
2. 写入 MBR + Limine BIOS 引导器（官方 hdd 布局：stage1 + stage2 两半）；
3. 创建 UEFI ESP（FAT32，128MB）：EFI/BOOT/BOOTX64.EFI + limine.conf + kernel.elf
   ——UEFI 启动经 GOP 拿 framebuffer；
4. 创建 ext2 系统分区：kernel.elf、limine.conf、limine-bios.sys、dict.dat
   ——BIOS 启动经 VBE 拿 framebuffer；
5. 提示关闭电源后从该硬盘重新启动——安装介质使命结束。

验证要点（`test-install.sh` 自动检查）：
- MBR 双分区（0xEF ESP @LBA2048 + 0x83 系统 @LBA264192）；
- ESP 用 fsck.fat/mtools 校验（FAT32：512B 簇、精确 FATSz32、FSInfo 签名、
  目录含 "." ".."、长文件名 LFN）；
- debugfs 列出 ext2 系统分区 4 个文件；
- BIOS 直启进 shell（framebuffer@0xffff8000fd000000，VBE 路径）；
- UEFI 直启进 shell（framebuffer@0xffff800080000000，GOP 路径）。

注意事项（QEMU TCG 环境）：
- 安装盘建议 `cache=unsafe`；QEMU 主循环需 monitor 命令定期唤醒（脚本已内置），
  否则长写入（28MB kernel.elf）可能挂起；
- 安装完成后内核即重启，安装路径不释放 kmalloc 内存（跳过 buddy 合并的偶发崩溃，
  见 kernel/install/install.c 顶部说明）。

### 阶段一验证结果（QEMU 10.2 + OVMF，`scripts/test-qemu.sh`）

```
[Nova] boot: kmain entered
[Nova] bootloader: Limine 8.7.0
[Nova] framebuffer: address=0xffff800080000000 width=1920 height=1080 pitch=7680 bpp=32 model=1
[Nova] Phase1 OK: red pixel drawn at (0,0)
[Nova] entering infinite hlt loop
PIXEL_TEST PASS   # 1920x1080，pixel(0,0)=(255,0,0)，pixel(12,12)=(255,0,0)
```

### 阶段二验证结果

```
[Nova] boot: kmain entered
[Nova] bootloader: Limine 8.7.0
[Nova] font: Terminus PSF2 8x16
[Nova] terminal: 80x25 initialized
[Nova] Phase2 OK: terminal rendered
PIXEL_TEST PASS   # 1920x1080 终端画面：886 文本像素 + 63 绿色[OK]像素
```

屏幕显示（左上角 80x25 终端）：
```
Nova v0.1 - [OK] Framebuffer initialized        ← [OK] 绿色
[INFO] Resolution: 1920x1080, pitch=7680, bpp=32
[INFO] Font: Terminus 8x16, 256 glyphs (PSF2)
[OK] Terminal initialized (80x25)
fmt test: -42 12345 beef BEEF 0xffffffff80000000 PSF2 Q %
Nova v0.1 - [OK] Boot sequence complete, entering idle loop
```

阶段二截图：`build/terminal-shot.png`（左上 700x320 区域放大 3 倍）。

### 阶段三验证结果

```
[Nova] GDT: loaded (null/kcode/kdata/ucode/udata)
[Nova] IDT: 256 vectors installed
[Nova] ISR 0x80 handler: rax=42 rbx=0xdead    ← INT 0x80 往返寄存器零差错
[Nova] Phase3 OK: GDT+IDT+INT0x80 verified
PIXEL_TEST PASS   # 终端继续渲染，1300 文本像素 + 84 绿色像素
```

阶段三截图：`build/phase3-shot.png`。

模块结构：
- `kernel/gdt/` - gdt.h/gdt.c（5 描述符构建）+ gdt_flush.S（lgdt + 段寄存器刷新 + 远返回换 CS）
- `kernel/idt/` - idt.h/idt.c（256 中断门 + 分发 + 默认 stub）+ isr.S（pushaq/popaq、二分递归宏生成 256 stub 与地址表）
- `kernel/lib/uart.c` / `printf.c` - 串口与终端共用的 kvprintf 格式引擎

### 阶段四验证结果

```
[Nova] PIC: remapped IRQ0-15 to vectors 0x20-0x2F, all masked
[Nova] keyboard: IRQ1 unmasked, handler installed
[Nova] Phase4 OK: interrupts enabled, echo loop running
PIXEL_TEST PASS (keyboard echo verified)
# sendkey 综合注入后回显字形逐像素校验（全部 128/128）：
#   caps_lock+a -> 'A'（CapsLock 大小写）
#   kp_1 kp_add kp_2 -> '1' '+' '2'（小键盘，NumLock 默认开）
#   kp_enter -> 换行
#   shift-x -> 'X'（Shift 组合）
# Ctrl+Alt+Del：serial 输出 "[Nova] reboot requested" -> OVMF 重新引导
```

阶段四截图：`build/phase4-shot.png`、`build/phase4b-shot.png`。

光标增强（阶段四打磨）：
- 终端维护 grid[25][80] 字符缓存，光标以**反色块**渲染（隐藏时按缓存还原字符，
  不破坏下层文本）
- 闪烁由 `kernel/timer/timer.c`（PIT 通道 0，1000Hz，IRQ0）驱动，周期 250ms；
  主循环 `terminal_cursor_tick(timer_get_ticks())` 翻转
- 验证：连续 5 张截图（间隔 200ms）光标格中心像素严格交替
  (0,0,0) <-> (170,170,170)，CURSOR_BLINK PASS
- 该 PIT 时钟同时为阶段十的时间片抢占调度铺路

方向键与行编辑（阶段四打磨）：
- **←/→**：光标在当前行内水平移动（不删除字符），上下方向键不启用垂直移动
- **插入式输入**：光标处打字时右侧字符整体右移（如 "abc" 移到 'b' 前输入 X -> "aXbc"）
- **Delete**：删除光标处字符并左移收拢；**退格**：删除光标前一字符并左移
- 验证：a b c -> ←← -> X(插入) -> 退格 -> Y(插入) 得到 "aYbc"，
  字形 128/128 + 删除位置确认为空
- 顺带修正：主键盘 Delete 键（0x53 非扩展）此前在 NumLock 开时被误映射为
  小键盘句点 '.'，现正确映射为 Delete；小键盘 KP.（0xE0 0x53）仅在 NumLock 时输出 '.'

光标/行编辑 bug 修复（本轮）：
1. **幽灵光标**：光标移动/换行后旧位置反色块未清除。修复：putchar 开头先以
   正常色重绘当前光标格，再执行操作，最后在新位置画光标。
2. **右移越界**：右方向键可无限移动到行尾之外。修复：新增 line_content_end()，
   光标最多移到当前行最后一个字符之后。
3. **LED 同步竞态（关键）**：0xED 命令的 ACK 被 IRQ 抢读、且 keyboard_service
   的"清残留"循环会吞掉真实按键扫描码（实测 a b c 变 c a）。修复：
   - ACK 改由 IRQ 状态机消费（kb_cmd_state：0 空闲 / 1 等 ACK / 2 已收）
   - 等待 ACK 期间遇到的意外字节按正常扫描码处理，绝不丢弃
   - 移除会吞键的输出缓冲 drain
   修复后扫描码流零丢失、零 ACK 泄漏（逐字节串口追踪验证）。
4. **printf 引擎补全**：kvprintf 新增 %0NX 零填充与宽度支持（调试 %02x 时发现）。

Windows 验收注意：
- QEMU 窗口需**先点击内部**获得键盘焦点再打字
- Windows 系统级拦截真实的 Ctrl+Alt+Del（SAS），请用 QEMU 菜单
  Machine -> Send Ctrl+Alt+Del 触发软重启

模块结构：
- `kernel/pic/` - pic.h/pic.c：8259A 初始化（ICW1-4，IRQ0-15 -> 0x20-0x2F）、OCW1 屏蔽、EOI
- `kernel/input/` - keyboard.h/keyboard.c：Set 1 扫描码转 ASCII、CapsLock（仅字母）、
  NumLock + 小键盘（0-9 + - * / . KPEnter）、0xE0 扩展码、Shift/Ctrl/Alt、
  键盘 LED 同步（8042 0xED）、256 字节环形队列、Ctrl+Alt+Del 重启
- `kernel/lib/io.h` - 内联端口 I/O（outb/inb/outw/inw/outl/inl/io_wait/sti/cli/hlt/pause）

### 阶段五验证结果

```
[Nova] Physical Memory Manager initialized
[Nova] PMM: total=510MB used=7MB (1%) free=128900 pages bitmap@0x1000
[Nova] PMM self-test: alloc+free, leak=no
PIXEL_TEST PASS   # 键盘回显回归全通过
```

阶段五截图：`build/phase5-shot.png`。

模块结构：
- `kernel/mm/` - pmm.h/pmm.c：Limine memmap 可用区过滤 -> 4KB 页位图 ->
  pmm_alloc_page / pmm_free_page（assert 已分配）/ pmm_alloc_pages(连续·首次适应) /
  pmm_free_pages / pmm_print_usage / pmm_free_page_count
- `kernel/include/assert.h` + `kernel/lib/panic.c`：断言失败红字 PANIC + 停机

### 阶段六验证结果

```
[Nova] Buddy allocator initialized
[Nova] Buddy: orders 0-10 pool=503MB free=503MB (100%)
        [0]=11 [1]=8 [2]=13 [3]=8 [4]=11 [5]=4 [6]=5 [7]=7 [8]=5 [9]=6 [10]=120
[Nova] Buddy self-test: 100 blocks, leak=no
PIXEL_TEST PASS   # 键盘/终端回归零破坏
```

阶段六截图：`build/phase6-shot.png`。

模块结构：
- `kernel/mm/buddy.h`/buddy.c：order 0-10 空闲链表；buddy_init（逐页入池 +
  buddy_free 级联合并构建最优池）；buddy_alloc（分裂）；buddy_free（合并）；
  order_to_size / size_to_order；buddy_print_stats / buddy_free_pages
- `kernel/lib/rand.h`/rand.c：xorshift32 伪随机（压力测试）

### 阶段七验证结果

```
[Nova] Paging enabled: CR3=0x1fec4000 kernel 0xffffffff80000000->0x1a0f1000 hhdm=0xffff800000000000
[Nova] vmm test: ident ok, kernel map ok, fb=0xffff800080000000, alloc r/w ok
[Nova] PIC/keyboard/timer 初始化继续正常工作（自有页表下运行）
PIXEL_TEST PASS   # 键盘回显回归零破坏
```

阶段七截图：`build/phase7-shot.png`。

模块结构：
- `kernel/mm/vmm.h`/vmm.c：四级页表（packed 位域 + _Static_assert sizeof==8）、
  identity[0,4GB)/HHDM 2MB 大页、内核 4KB 映射、vmm_alloc_page/vmm_map_page/
  virt_to_phys/phys_to_virt、CR3/CR4.PAE/EFER.LME+NXE/CR0.PG 切换

### 阶段八验证结果

```
[Nova] kmalloc initialized
[Nova] kmalloc self-test: 100 objects, leak=no
[Nova] kmalloc: active=0 alloc=423807B hits=16 misses=33 [32]=128 [64]=64 [256]=16 [512]=8 [1024]=12 [2048]=14 [4096]=19
PIXEL_TEST PASS   # 键盘/终端回归零破坏
```

阶段八截图：`build/phase8-shot.png`。

模块结构：
- `kernel/mm/kmalloc.h`/kmalloc.c：8 级 slab 对象池（32..4096 字节，空闲链表存对象
  头部）+ 大对象（>4096 走 buddy，页首 8 字节记录 order）；krealloc/kcalloc/
  kmalloc_size/kmalloc_active_objects/kmalloc_print_stats

### 阶段九验证结果

```
[Nova] task 1 exited (code 0)
[Nova] task 2 exited (code 0)
[Nova] scheduler: round-robin done, back to idle
[TEST] scheduler: ABABABABAB done, round-robin OK (1ms)
PIXEL_TEST PASS   # 键盘/终端回归零破坏
```

阶段九截图：`build/phase9-shot.png`。

模块结构：
- `kernel/sched/` - sched.h/sched.c（struct task、task_init/create_task/schedule/yield/
  task_exit、FIFO 就绪队列轮转、idle 借用启动栈）+ switch_to.S（pushaq/popaq
  栈式切换、task_trampoline 退出蹦床）

关键工程要点（阶段九）：
32. **栈式切换**：switch_to 将全部 GPR（pushaq）压入 prev 栈并记录 regs.rsp，
    切到 next->regs.rsp 后 popaq+ret —— 任务从上次让出点继续。
33. **初始栈帧**（低->高）：[15×0][entry][trampoline] —— popaq 后 ret 进 entry；
    entry 返回后 ret 进蹦床自动 task_exit。压栈顺序反了会 ret 到垃圾地址
    （实测 RIP=0x1000 #UD，已修复）。
34. **轮转调度**：schedule 将运行中任务放回队尾、取队首切换；队列空回 idle
    （借用启动栈，控制权回到 kmain）。
35. 测试基建：验收脚本串口改到 WSL ext4（/tmp）——drvfs 串口停滞会拖慢
    启动（uart 超时逐字符等待）导致按键打在未完成的启动上（曾误判为丢键）。

关键工程要点（阶段八）：
27. slab 空闲对象链表：空闲对象头 8 字节存下一空闲地址（对象即链表节点）。
28. kfree 路径识别：扫描各缓存对象页（指针所属物理页匹配）-> slab；否则按
    大对象页首 order 释放 —— 无需用户传 size。
29. 对象虚拟地址 = 物理 + hhdm（vmm 页表 HHDM 直接映射覆盖全部物理内存）。
30. 命中率统计：命中=从现有空闲对象分配，miss=新开 slab 页（首次触达缓存）。
31. 大对象页首 8 字节头（kmalloc 返回 +8 地址），krealloc 经 kmalloc_size
    读取实际大小决定是否搬移。

关键工程要点（阶段七）——本阶段排障挖出三个连环坑：
23. **页表页必须经 HHDM 访问**：Limine 的 identity 映射不覆盖高位物理地址，
    用 phys 当 vaddr 写页表会落到错误物理页（QEMU xp 物理内存与内核读
    不一致暴露）。buddy 空闲链表节点同步改为 HHDM 访问。
24. **页表项必须 8 字节**：位域 1+1+1+1+1+1+1+1+1+3+40+1=53 位 + packed
    → sizeof=7，只有第 0 项偏移正确，其余全部错位。补 11 位保留位 +
    _Static_assert(sizeof==8) 防复发。
25. **address 位域存页帧号（phys>>12）**：硬件把 bits 12-51 当帧号，
    存完整物理地址会变成 <<12 的错误地址（0x80a000 -> 0x80a000000）。
26. 调试方法论：QEMU `xp` 读物理内存 vs 内核自读对比定位别名页问题；
    `-d int` 异常链 + `info registers` 抓 RIP；`-serial` 用 WSL ext4
    （drvfs 文件停滞会触发 uart 超时丢字符，误导排障）。

关键工程要点（阶段六）：
18. **池构建**：逐页 pmm_alloc_pages(1) + buddy_free(phys,0) 级联合并 ——
    天然产出最大对齐块（实测 120 个 4MB 块 + 各阶小块），无需显式合并 pass。
19. **性能教训**：首次适应必须带扫描游标（scan_hint），否则 buddy_init 逐页
    全表扫描 = O(n²)（实测 30 秒+ 未完成 -> 修复后瞬间完成）；
    pmm_alloc_page 同样需要（buddy_init 改走 pmm_alloc_pages(1)）。
20. **uart 超时**：串口后端停滞（QEMU 文件输出被阻塞）会让 THRE 忙等无限
    挂起 -> uart_putc 加 100K 次超时，卡死时丢弃字符而非挂死内核。
21. 空闲链表节点存放在空闲块自身头部；buddy 地址 = phys ^ size。
22. 测试随机 order 0-8 + 降级容错（某阶无块时降阶重试），逆序释放验证无泄漏。

关键工程要点（阶段五）：
14. 位图置于最低可用区（>=1MB）头部，物理地址 + hhdm_offset 得到可写虚拟地址；
    位图自身页、页 0、1MB 以下固件区强制保持已用。
15. 仅 Limine 类型 == USABLE 的区域进入分配池；内核/模块/帧缓冲/保留区不释放。
16. pmm_free_page 通过 assert 校验（页对齐、范围、确实已分配），错误路径立即 PANIC。
17. 连续分配用首次适应扫描；free_pages 计数器用于泄漏自检（分配后释放回到基线）。

关键工程要点（阶段四）：
11. 环形队列用 uint8_t 索引自然回绕（256 字节），单生产者单消费者各写一端，
    单核下无需加锁；多核阶段补充内存屏障。
12. 长模式无法直接跳转 16 位复位向量 0xFFFFFFF0：reboot() 依次尝试
    8042 复位脉冲（0x64, 0xFE）-> ACPI 复位（0xCF9, 0x06）-> 0xFFFFFFF0 兜底
    （QEMU 下三倍故障等效重启）。
13. 主循环 hlt 等待中断而非忙轮询，QEMU 下 CPU 占用为零。

关键工程要点（阶段三）：
7. 异常（vec<32）默认 stub 打印寄存器转储后停机，避免 iretq 回到故障指令死循环；
   硬件/软件中断（vec>=32）打印后正常返回。
8. GAS 宏递归生成 256 个 stub：普通递归 256 层超出嵌套上限，改用二分递归（深度 8）；
   参数求值需 .altmacro 的 %expr 语法。
9. 同名源文件冲突：kernel/gdt/gdt.S 与 gdt.c 目标文件同名，汇编文件改名 gdt_flush.S。
10. INT 0x80 测试验证 pushaq/popaq 寄存器往返（rax=42、rbx=0xdead 完整保留）。

关键工程要点（阶段一/二）：
1. **Limine v8.7 配置格式已变更**：须用 `limine.conf`（新格式 `key: value`、条目 `/Title`、
   `boot():/path`）；`limine.cfg` 旧格式会触发 20 秒警告。
2. **Limine 引导协议默认关闭 SSE**：gcc -O2 生成的 SSE 指令会触发 #UD，
   boot.S 入口必须启用 SSE（CR0.EM=0/MP=1、CR4.OSFXSR/OSXMMEXCPT=1、初始化 MXCSR）。
3. **Limine 请求必须带 `section(".requests")`** 属性，且需 `.requests_start_marker` /
   `.requests_end_marker` / `LIMINE_BASE_REVISION(0)`，否则引导器不响应请求。
4. 帧缓冲像素格式按通道位移（red/green/blue_mask_shift）构造颜色（本机实测 red_mask_shift=16）。
5. 字体源：Ubuntu 25.04 各包均不再携带 PSF2 版 Terminus，最终取 Arch 包
   `ter-116n.psf`（8x16 unicode，即 ter-u16n 同规格），经
   `scripts/psf1-to-psf2.py` 无损转 PSF2；`scripts/gen-font.py` 转 C 数组。
6. `terminal_printf` 支持 %d %u %x %X %p %s %c %% 及 l/ll 修饰；
   滚屏用 memmove 帧缓冲；颜色按帧缓冲通道位移转换（0xRRGGBB → 像素值）。

### 阶段二十验证结果（最终阶段：Shell / 文档 / CI）

```
[Nova] shell selftest: fs/redirect/pipe/env/builtins OK
nova$ ps
PID  STATE  CPU  TICKS  NAME
...
nova$ ls -l /
d        0 pkgs
f        0 shell.txt
d        0 shd
...
nova$ echo hello
hello
nova$ help
Nova CLI shell v0.1 (phase 20)
fs: ls cd pwd cat echo touch mkdir rm rmdir cp mv stat head tail wc find df du
proc: ps top kill sleep exit pidof
sys: uname meminfo cpuinfo uptime version whoami date dmesg clear reset reboot halt yes
pkg: nvp(list/info/install/remove/files/cat) repo(update/list/install)
env: env set  | syntax: | > >> & $VAR 'q' "q"
nova$ nvp list
demo: 3
webdemo: 2
nova$ repo list
repo (2 available):
util
webdemo
nova$
```

`scripts/test-qemu.sh` 全量 13 项检查通过（SMP/USERMODE/TMPFS/ATA/MBR/
EXT2/NVP/NVPMGR/NET/SHELL/WENYUAN/WENYUAN_SHOT/CLI+PIXEL），
WENYUAN_GLYPH 为参考性 NOTE（宿主 FT 2.13.2 与内核 2.13.3 hinting
版本差异，逐像素匹配不做 PASS/FAIL），连续多次稳定。

阶段二十内容：
- **Shell**（kernel/cli/shell.c）：40+ 内建命令，管道 `|`（最多 3 级）、
  重定向 `>`/`>>`、后台 `&`、`$VAR` 展开、单双引号、cwd/相对路径；
  ps/top/kill 基于调度器新增的全局任务注册表（sched_count/sched_get/
  sched_kill）；dmesg 基于新增内核日志环（kernel/lib/klog.c）。
- **VFS 扩展**：vfs_stat / vfs_readdir（ls -l / stat / find / du 等）。
- **文档**：docs/ARCHITECTURE.md、docs/BUILD.md、docs/TESTING.md。
- **CI**：.github/workflows/ci.yml（Ubuntu 容器构建 + 全量 QEMU 验收）。
- 关键 bug 修复：normalize_path 首组件缺失根斜杠导致 vfs_create
  -EINVAL；管道缓冲读取需"消费"（否则 stdin 命令输出混叠）；
  verify-cli.py 扫描起始行随 boot 行数增长下调（15→10）。

### 阶段二十增强：文渊黑体（FreeType）+ 中文显示 + 字体切换

- **FreeType 2.13.3 集成**（third_party/freetype/）：裁剪模块
  base/truetype/cff/sfnt/psaux/pshinter/psnames/smooth/raster/autofit，
  编译选项 `-DFT_CONFIG_OPTION_DISABLE_STREAM_SUPPORT`（内核无文件流）；
  `ftsystem_kernel.c` 把 FreeType 内存管理接到 kmalloc/kfree/krealloc
  （原版 ftsystem.c 依赖 stdio 且 ftstdlib.h 会覆盖 `-D` 宏映射，不可用）；
  `ftdebug_kernel.c` 无条件提供 FT_Message/FT_Panic/FT_Throw/FT_Trace_*；
  ftoption.h 关闭 USE_ZLIB/USE_LZW/ENVIRONMENT_PROPERTIES。
- **文渊黑体**（kernel/font/WenYuanSansSCVF.ttf，27MB）：文源黑体
  WenYuan Sans SC（思源黑体衍生，OFL 许可），经 objcopy 嵌入
  .rodata.font 段；`kernel/font/wenyuan.c` 用 FT_New_Memory_Face 加载，
  16px 灰度渲染（`FT_LOAD_TARGET_LIGHT` 轻 hinting，字形清晰不糊）+
  1024 组 x 2 路字形位图缓存 + 启动预热常用字形（QEMU TCG 提速；
  直接映射缓存会让低 11 位相同的汉字互相覆盖成错字）。
- **终端 UTF-8**（kernel/terminal/terminal.c/h）：grid 升级为 uint32_t
  码点网格，terminal_putchar 内置 UTF-8 解码状态机；CJK 宽字符占 2 列
  （TERM_WIDE_TAIL 标记右半格，光标前进/移动/退格均跳过右半格）；
  灰度抗锯齿 alpha 混合 fg/bg。
- **字体分工（用户定稿方案）**：wenyuan 模式下**英文/数字/符号用 PSF2
  像素字体（8x16 等宽）**，**中文（CJK）用文渊黑体 FreeType 灰度
  抗锯齿（16px 宽占 2 列）**；布局等宽（窄 8px / 宽 16px），换行按
  80 列；`font set mono` = 全部像素（中文退化为 '?'）。
- **行高 22px（TERM_ROW_H）**：16px 字形 + 基线以下最多 5px descender
  + 1px 余量，p/q/g/j/y 完整显示且绝不侵入下一行（blit 行底裁剪
  作防御）；滚动后全行重绘，光标反色块不残留。
- **字体切换命令**：`font`（当前）、`font list`、`font set mono|wenyuan`；
  启动即默认文渊黑体，中文欢迎行在 Shell 自检后、CLI 提示符前输出：
  "你好，Nova！文渊黑体渲染中文成功。"（屏幕像素画逐字形确认：
  中英文字形完整、比例正确、无重叠、无截断、无杂色残留）。
- 联动修复：内核镜像从几 MB 涨到 28MB，vmm_init 高半区映射由固定 8MB
  改为按链接器符号 `_kernel_end` 映射整个镜像（否则切换 CR3 后访问
  字体段触发 #PF）。
- 联动修复：任务栈 4KB→64KB（FreeType autofit 的 `af_autofitter_load_glyph`
  栈帧含 96 个 embedded AF_PointRec 约 16KB；原 4KB 任务栈溢出写坏相邻
  kmalloc 内存，导致 FreeType 释放垃圾指针 / buddy 对齐断言失败）。
  另外 `libc_extra.c` 补齐 FreeType 依赖的 libc 符号（__isoc23_strtol /
  qsort / strstr / memchr / _setjmp / __longjmp_chk）。

### 阶段二十增强（续）：多盘 + 容量命令

- **ATA 四设备枚举**（kernel/ata/ata.c）：主/从通道（0x1F0/0x170）x
  主/从设备共 4 个槽位，逐个 IDENTIFY 探测（空槽 st=0 正确跳过）。
  - **关键修复：SRST 必须等待 BSY 清零再发命令**。QEMU 的 SRST 由
    主循环 bottom-half 异步完成，复位完成前命令块寄存器写入会被丢弃
    （dev2 曾因此随机 "IDENTIFY no DRQ"）；现在复位后轮询 BSY
    （每 1024 次 inb 让步一次主循环），再选择设备、再轮询、再 IDENTIFY，
    主/从设备一视同仁，枚举稳定。
- **ext2 多实例**（kernel/ext2/ext2.c/h）：`ext2_fs[EXT2_MAX_FS=4]`
  槽位数组，`ext2_mount_slot(slot, part_lba, ata_dev)` 记录每个卷的
  所属 ATA 设备，`ext2_select(slot)` 自动选中对应 ATA 设备后再读盘
  （避免切换盘后读到上一块盘的错误扇区）；兼容旧 API `ext2_mount()`。
- **VFS 盘表**（kernel/fs/vfs.c/h）：`struct drive {letter, label, root,
  total_bytes, free_bytes, used_bytes}`，`vfs_mount_drive()` 挂盘；
  路径解析支持 `X:` 盘符前缀，`vfs_set_current_drive()` 切换当前盘。
- **多盘挂载**（kernel/kernel.c）：遍历全部 ATA 槽位，读 MBR 找
  0x83 分区，ext2 挂载为 VFS 盘；盘符自 `D:` 起（C: 为 tmpfs 系统盘）。
  槽位 0 保留给阶段十六/十七的 ext2 测试。
- **Shell 命令**：
  - `disk` / `df`：列出全部已挂载盘（盘符、卷标、总容量、已用、
    剩余，MB 向上取整，当前盘标 `*`）
  - `cd X:`：切换当前盘并回到该盘根（提示符变为 `nova:X:/$ `）；
    `cd E:/dir` 等带盘符路径亦可
  - `cat`/`ls`/`pwd` 等全部命令按当前盘解析路径
- **测试盘**：scripts/prepare-ext2-disk.sh 额外生成 build/test2.img
  （48MB，MBR + ext2 24MB，含 data.txt 与 e2data/notes.txt）；
  scripts/test-qemu.sh 以 piix4-ide 双通道挂载（disk0@ide0.0 主、
  disk1@ide0.1 从），MULTIDISK_TEST 校验 "2 drive(s) mounted" +
  D:/E: 挂载成功。scripts/test-multidisk.sh 提供免截图的多盘快速验收。
