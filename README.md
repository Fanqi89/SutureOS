# SutureOS · 缝合怪系统

> 一个将 15 个开源自制操作系统（Hobby OS）内核的**优点模块**缝合而成的 x86_64 实验内核。
> 整体以 **GPL-3.0** 发布，每个取材模块均保留原项目版权与协议声明。

## 这是什么

SutureOS（中文名：缝合怪系统）不是从零发明的新内核，也不是简单拷贝——它从 15 个爱好者操作系统中**各取一块最能打的内核代码**（启动层、内存管理、中断设施、VFS、调度……），做"排异处理"（符号去冲突、架构统一到 x86_64、依赖替换、协议合规）后缝合为一个**能真实启动、能通过全部启动自检**的内核，并用 QEMU 实测验证。

```
启动 (multiboot1 → 长模式)
  │  boot.S：64位启动段结构 ← Cinux
  ▼
串口控制台 ← PlantOS serial.c
  ▼
自检记分板 [selftest] ──→ 串口日志 ──→ scripts/run-qemu.ps1 自动判定
  │
  ├─ 页帧分配器   ← MoeOS      (page.rs → C)
  ├─ 内核堆       ← Lemis      (heap.c)
  ├─ Slab 分配器  ← SpiritFoxOS(slab.c)
  ├─ LIST 链表宏  ← Phobos     (queue.h)
  ├─ 红黑树       ← CoolPotOS  (rbtree.c)
  ├─ FIFO 环形缓冲← Helo OS    (haribote/fifo.c 谱系，重写)
  ├─ 上下文切换   ← Cinux      (context_switch.S)
  ├─ GDT/TSS/IDT  ← VimtuOS    (x86_64.cpp)
  ├─ 中断分发器   ← MOOS       (interrupts_asm/IDT.cs → C)
  ├─ 键盘/PIT     ← MandelbrotOS(kbd.c / pit.c)
  ├─ ACPI/MADT/HPET ← HanOS    (acpi.c/madt.c/hpet.c)
  ├─ IOAPIC       ← NAOS       (io_apic.cc → C)
  ├─ VFS 骨架     ← XJ380      (vfs.h/vfs.cpp 接口设计)
  └─ 桥接胶水/记分板 = SutureOS 原创 (GPL-3.0)
```

## 取材溯源总表（谁家的什么优点进了这个内核）

| # | 来源系统 | 协议 | 缝入的模块 | 原始文件 | 移植文件 | 说明 |
|---|---------|------|-----------|----------|----------|------|
| 1 | **CoolPotOS** | MIT | 红黑树 | `src/util/rbtree.c` + `src/include/rbtree.h` | `lib/rbtree.c`, `include/stitch/rbtree.h` | Linux 风格 rb_insert/rb_erase，零依赖数据结构 |
| 2 | **SpiritFoxOS** | GPL-3.0 | Slab 分配器 | `kernel/src/mm/slab.c` | `kernel/mm/slab.c` | 8 档 size-class + bitmap 空闲标记 |
| 3 | **Cinux** | MIT | 启动 64 位段 + 上下文切换 | `kernel/arch/x86_64/boot.S`, `context_switch.S` | `boot/boot.S`(结构), `arch/x86_64/context_switch.S` | CpuContext 偏移静态断言锁定 |
| 4 | **Phobos** | GPL-3.0 | LIST 链表宏 | `mainline/kernel/sys/queue.h` | `lib/phobos_queue.h` | 纯 C 的 LIST_* 宏段，版权头原样保留 |
| 5 | **MOOS** | Unlicense | 中断 stub + 分发器 | `NativeLib/interrupts_asm.asm`, `Kernel/Misc/IDT.cs` | `arch/x86_64/isr_stubs.S`, `kernel/interrupts/intr.c` | MASM→GAS 重写；C# 分发表设计直译成 C |
| 6 | **MoeOS** | GPL-3.0 | 页帧分配器 | `src/mem/page.rs` | `kernel/mm/pmm.c` | Rust→C 1:1 逻辑移植（first-fit + 边界标记） |
| 7 | **MandelbrotOS** | MPL-2.0 | 键盘解码 + PIT | `src/kernel/kbd.c`, `src/kernel/pit.c` | `kernel/drivers/mb_kbd.c`, `mb_pit.c` | scancode set1 状态机；文件保持 MPL-2.0 |
| 8 | **HanOS** | MIT | ACPI/MADT/HPET | `kernel/arch/x64/{acpi,madt,hpet}.c` | `arch/x86_64/{acpi,madt,hpet}.c` | RSDP 扫描 + 表解析 + HPET 频率测量 |
| 9 | **Helo OS** | GPL-3.0 | FIFO32 环形缓冲 | `HeloOS/Helo OS 2019.0.0.2/haribote/fifo.c` | `kernel/ipc/fifo.c` | Haribote 谱系，接口语义借鉴+干净重写 |
| 10 | **XJ380** | Apache-2.0 | VFS 接口骨架 | `include/fs/vfs/vfs.h`, `driver/fs/vfs/vfs.cpp` | `kernel/vfs/vfs.c` | vfs_callback 函数指针表 + 节点树设计 |
| 11 | **NAOS** | BSD-3-Clause | IOAPIC 驱动 | `naos/src/kernel/arch/io_apic.cc` | `arch/x86_64/ioapic.c` | C++→C，MMIO 重定向表编程 |
| 12 | **PlantOS** | MIT | 串口驱动 | `src/kernel/drivers/general/serial.c` | `arch/x86_64/serial.c` | 16550 初始化，全内核早期输出 |
| 13 | **Lemis** | GPL-3.0 | 内核堆 | `0.5v+0.2.1v/src/system/kernel/other/heap.c` | `kernel/mm/heap.c` | 空闲链表 kmalloc/kfree + 相邻块合并 |
| 14 | **VimtuOS** | GPL-3.0 | GDT/TSS/IDT 门层 + 端口 I/O | `kernel/x86_64.cpp`, `kernel/port.h` | `arch/x86_64/gdt_idt.c`, `include/stitch/io.h` | 16 字节 IDT 门、TSS 描述符 |
| 15 | **Helo OS 4.1** | GPL-3.0 | （工具仓库）`bim2hel` 镜像转换工具 | `HeloOS4.1/bim2hel` | `contrib/`（源保留引用） | 与 Helo OS 同一系统，仓库无内核源，贡献其工具 |

> 详细差异说明（每个文件改了什么）见 [docs/provenance.md](docs/provenance.md)；
> 协议合规声明见 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)。

## 目录结构

```
缝合怪系统/
├── README.md                  ← 本文件（系统介绍 + 溯源总表）
├── LICENSE                    ← GPL-3.0
├── THIRD_PARTY_LICENSES.md    ← 第三方协议/版权逐条声明
├── boot/                      ← multiboot1 启动 + 链接脚本
│   ├── boot.S
│   └── linker.ld
├── arch/x86_64/               ← 体系结构层
│   ├── serial.c               ← PlantOS
│   ├── context_switch.S       ← Cinux
│   ├── gdt_idt.c              ← VimtuOS
│   ├── isr_stubs.S            ← MOOS
│   ├── acpi.c madt.c hpet.c   ← HanOS
│   └── ioapic.c               ← NAOS
├── kernel/
│   ├── main/                  ← kmain 装配 + 自检记分板（原创）
│   ├── mm/                    ← pmm(MoeOS) heap(Lemis) slab(SpiritFoxOS)
│   ├── interrupts/            ← intr.c 分发器 (MOOS)
│   ├── drivers/               ← mb_kbd/mb_pit (MandelbrotOS)
│   ├── ipc/                   ← fifo (Helo OS)
│   ├── vfs/                   ← vfs (XJ380)
│   └── sched/                 ← 协作调度胶水 + Cinux 上下文切换
├── lib/                       ← kprintf/string(原创) rbtree(CoolPotOS) phobos_queue.h(Phobos)
├── include/stitch/            ← 公共头（类型/IO/控制台/自检）
├── docs/
│   ├── architecture.md        ← 架构说明（各子系统来自哪家）
│   ├── provenance.md          ← 取材溯源详细差异记录
│   ├── build-and-test.md      ← 构建与测试步骤
│   └── verification-report.md ← QEMU 功能验证报告（排异检查结果）
├── scripts/
│   ├── build.ps1              ← 编译 + 链接
│   └── run-qemu.ps1           ← QEMU 启动 + 串口自检解析
├── tests/                     ← 主机侧辅助检查（符号冲突扫描等）
└── build/                     ← 产物（kernel.elf、串口日志）
```

## 构建与运行（需要 MSYS2 clang + x86_64-elf binutils + QEMU）

```powershell
# 1. 编译
.\scripts\build.ps1
# 2. QEMU 启动并自动判定自检结果
.\scripts\run-qemu.ps1
```

预期串口输出尾部：

```
[selftest][moe_pmm] PASS
[selftest][lemis_heap] PASS
...
[stitch] selftests: 12/12 passed
[stitch] halted: kernel boot chain verified
```

## 设计约束与排异检查（Rejection checks）

缝合 15 家代码会遇到四类"排异反应"，本项目逐类设防：

1. **符号冲突** — 所有移植模块导出符号强制加来源前缀（`moe_ / lemis_ / sfox_ / vim_ / moos_ / mbos_ / hanos_ / naos_ / xj_ / cinux_ / sched_ / helos_`），内部函数 static；链接前用 `tests/symbol-scan.ps1` 扫描重复定义。
2. **架构冲突** — 源仓库横跨 i386/riscv64/loongarch64/C#/Rust/C++：只取架构无关算法（Rust→C）或 x86_64 对应层（C++→C），32 位代码统一按 64 位语义改写（指针宽度、对齐）。
3. **协议冲突** — 全部 15 家协议（MIT/GPL-3.0/MPL-2.0/Apache-2.0/BSD-3/Unlicense）均与 GPL-3.0 聚合兼容；MPL 文件保持文件级协议；每个移植文件带 Ported-from 头；详见 THIRD_PARTY_LICENSES.md。
4. **行为冲突** — 每个模块带启动期自检（[selftest] 记分板），全部 PASS 才算缝合成功；QEMU 实测串口日志为准（docs/verification-report.md）。

## 路线图（后期扩展为完整操作系统）

- [x] Phase-1：可启动缝合内核 + 14 模块自检（当前里程碑）
- [ ] Phase-2：开启中断（VimtuOS IDT + MOOS 分发 + HanOS HPET 时钟 + NAOS IOAPIC 路由），时钟驱动调度
- [ ] Phase-3：CoolPotOS EEVDF 调度器接入、Phobos buddy/slab 完整内存体系、SpiritFoxOS scheduler
- [ ] Phase-4：XJ380 VFS 完整化 + PlantOS/VimtuOS FAT32 + 真实磁盘（VirtIO/AHCI）
- [ ] Phase-5：系统调用（VimtuOS syscall64 设计）+ ring3 用户态 + musl
- [ ] Phase-6：图形（MandelbrotOS text/VBE + XJ380 console/字体 + VimtuOS fb/GUI）与网络（SpiritFoxOS TCP/IP）
- [ ] 发布：GitHub + GPL-3.0，附完整溯源与验证报告

## License

GPL-3.0（见 LICENSE），其中各移植文件的原始版权归各原作者/项目所有，详见 THIRD_PARTY_LICENSES.md。
