# SutureOS 架构说明（Architecture）

> 每一层标注代码来源：**[来源系统]** 或 **[原创]**（原创 = SutureOS 胶水，GPL-3.0）。

## 1. 总体分层

```
┌───────────────────────────────────────────────────────────┐
│ kernel/main/         装配层 kmain + 自检记分板      [原创] │
├───────────────────────────────────────────────────────────┤
│ kernel/vfs/          VFS 骨架（vfs_callback 表）  [XJ380]   │
│ kernel/sched/        协作调度胶水               [原创]     │
│                      └ 上下文切换 asm           [Cinux]    │
│ kernel/ipc/          FIFO32 环形缓冲            [Helo OS]  │
│ kernel/interrupts/   中断分发器 + IRQ 注册表    [MOOS]     │
│ kernel/drivers/      PS/2 键盘解码 + PIT        [MandelbrotOS]│
│ kernel/mm/           pmm / heap / slab          [MoeOS/Lemis/│
│                                              SpiritFoxOS]  │
├───────────────────────────────────────────────────────────┤
│ arch/x86_64/         串口            [PlantOS]             │
│                      GDT/TSS/IDT     [VimtuOS]             │
│                      ISR stubs (GAS) [MOOS]                │
│                      ACPI/MADT/HPET  [HanOS]               │
│                      IOAPIC          [NAOS]                │
│                      context_switch  [Cinux]               │
├───────────────────────────────────────────────────────────┤
│ boot/                multiboot1→长模式                    │
│                      64位启动段结构        [Cinux]         │
├───────────────────────────────────────────────────────────┤
│ lib/                 kprintf/string         [原创]         │
│                      红黑树                  [CoolPotOS]   │
│                      LIST 链表宏             [Phobos]      │
│ include/stitch/      类型/IO(端口IO [VimtuOS])/自检 [原创+  │
└───────────────────────────────────────────────────────────┘
```

## 2. 启动流程

1. **QEMU `-kernel` / GRUB multiboot1** 载入 `kernel.elf`（链接于 1 MiB，
   `boot/linker.ld`），CPU 以 32 位保护模式进入 `_start`。
2. `boot/boot.S`（32 位段）：保存 multiboot magic/信息指针 → 清 BSS →
   建 4 级页表（PML4/PDPT/PD，2 MiB 大页恒等映射前 1 GiB）→ 开 PAE/EFER.LME/
   分页 → `lgdt` + `ljmp` 进长模式。
3. **64 位段（结构沿 Cinux `boot.S`）**：装载段寄存器 → 设栈 →
   `kernel_main(magic, mbi)`。
4. `kernel_main`（装配层）：串口初始化 → 握手校验 → 依次调用 14 个模块的
   启动自检 → 记分板汇总 → 停机（等待后续阶段开中断）。

> 为什么用 multiboot1 而不是各家自带的 bootloader？因为缝合后的内核要
> **在 QEMU 里一条命令可复现启动**（`-kernel` 仅支持 multiboot1），同时
> 兼顾以后用 GRUB 制作可启动镜像（multiboot1 GRUB 同样支持）。各家自带的
> 引导（VimtuOS loader64、Cinux stage2、SpiritFoxOS EFI stub、XJ380 UEFI、
> NAOS multiboot2、CoolPotOS Limine 请求…）作为**取材记录**保留在溯源文档，
> 不进入缝合内核——否则 6 种引导协议互相冲突（排异-架构冲突第 1 例）。

## 3. 各子系统分工与"为什么是它家"

| 子系统 | 来源 | 为什么选它 |
|--------|------|-----------|
| 页帧分配 | MoeOS `page.rs` | 每页 1 字节标志 + first-fit 连续分配 + 边界释放，零依赖纯算法，RISC-V 无关 |
| 内核堆 | Lemis `heap.c` | 空闲链表 + 相邻块合并，代码量最小（115 行）易审计 |
| Slab | SpiritFoxOS `slab.c` | 8 档 size-class + bitmap，结构完整且无内联汇编/无 x86 依赖 |
| 链表 | Phobos `queue.h` | 内核级 LIST_* 宏（BSD 风格），全内核数据结构基础设施 |
| 红黑树 | CoolPotOS `rbtree.c` | Linux 风格 rb_insert/rb_erase，零 include 自包含 |
| FIFO | Helo OS `fifo.c` | 经典 fifo32 环形缓冲（Haribote 谱系，重写规避风险） |
| 上下文切换 | Cinux `context_switch.S` | 139 行保存 CpuContext + fs_base MSR，hpp/asm 双向 static_assert 锁定 |
| GDT/TSS/IDT 门层 | VimtuOS `x86_64.cpp` | 纯 64 位长模式完整实现，注释逐条讲 16 字节门坑 |
| 中断 stub/分发 | MOOS `interrupts_asm.asm` + `IDT.cs` | 宏生成 0-255 stub、向量+帧分发、注册表设计，比 32 位教程套路更贴近 x86_64 |
| 键盘 | MandelbrotOS `kbd.c` | scancode set1 解码状态机独立完整 |
| PIT | MandelbrotOS `pit.c` | 最小可用的时钟源（phase-2 与 HPET 二选一升级） |
| ACPI/MADT/HPET | HanOS `acpi.c/madt.c/hpet.c` | 无堆分配纯 MMIO/表解析，91/120/120 行小而完整 |
| IOAPIC | NAOS `io_apic.cc` | 与 HanOS 分工：HanOS 负责"表在哪"，NAOS 负责"往表里写" |
| VFS | XJ380 `vfs.h/vfs.cpp` | vfs_callback 函数指针表 + vfs_node 树的接口设计最完整（1605 行源，取核心骨架） |
| 串口 | PlantOS `serial.c` | 最小 16550 初始化，全内核早期输出的咽喉 |

## 4. 排异设计（冲突消解规则）

### 4.1 符号冲突（静态排异）
- 所有跨文件导出符号强制前缀：`moe_ / lemis_ / sfox_ / vim_ / moos_ / mbos_ /
  hanos_ / naos_ / xj_ / cinux_ / sched_ / helos_fifo32_`（Phobos `LIST_*` 宏与
  CoolPotOS `rb_*` 为命名域唯一，例外保留原名）。
- 模块内部函数一律 `static`。
- 链接前 `tests/symbol-scan.ps1` 用 nm 扫描全部 .o，任何全局符号出现在
  两个以上目标文件即判冲突（FAIL）。

### 4.2 架构冲突（移植排异）
- **位宽**：i386 源（Lemis/MandelbrotOS/Phobos）统一改 64 位指针算术；
  Lemis 堆原本 `long long*` 指针算术导致合并失效的 bug 在移植版修复。
- **语言**：Rust（MoeOS）、C#（MOOS）、C++（NAOS/XJ380/Cinux）全部落为 freestanding C；
  MASM（MOOS）→ GAS；NASM 参考 → GAS。
- **依赖替换**：各家的 klog/serial_puts → `console_printf`；alloc_page → `moe_pmm_*`；
  自旋锁 → 关中断内联锁（含 pushfq 保存标志）；limine/vmm_map → 恒等映射直接指针。

### 4.3 协议冲突（法律排异）
见 [THIRD_PARTY_LICENSES.md](../THIRD_PARTY_LICENSES.md)。要点：6 种协议全部与
GPL-3.0 聚合兼容；MPL-2.0 文件保持文件级协议；Haribote 谱系文件采用"借鉴+重写"。

### 4.4 行为冲突（运行时排异）
- 每模块开机自检（`[selftest]` 记分板），互不依赖中断/定时器；
- 自检采用**状态保存/恢复**隔离（如 pmm/slab 自检在静态 arena 上运行后恢复全局，
  防止测试污染真实分配器）；
- 潜在 bug 消解记录：SpiritFoxOS slab 双 free 判定反向、Lemis heap 合并失效、
  MoeOS page.rs alloc 窗口漏尾/越界——三处上游缺陷在移植版修复并在自检覆盖。

## 5. 阶段规划

- **Phase-1（当前）**：可启动 + 14 模块自检全绿 + 静态符号扫描通过。
- **Phase-2**：开中断（VimtuOS IDT → MOOS 分发 → NAOS IOAPIC 路由 →
  HanOS HPET 时钟），时钟驱动调度。
- **Phase-3+**：见 README 路线图。
