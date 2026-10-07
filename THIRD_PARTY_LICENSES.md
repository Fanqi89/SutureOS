# THIRD-PARTY LICENSES & COPYRIGHT NOTICES
# 第三方开源协议与版权声明（SutureOS / 缝合怪系统）

本项目整体以 **GNU General Public License v3.0**（见 LICENSE）发布。
以下 15 个上游开源项目的代码被以"移植/改写"方式引入本内核。所有上游协议
（MIT、GPL-3.0、MPL-2.0、Apache-2.0、BSD-3-Clause、Unlicense）均与
GPL-3.0 **聚合兼容**（aggregate compatible），即本项目整体可按 GPL-3.0 分发，
同时各移植文件保留其原始版权与协议义务（如下逐条列出）。

协议兼容性依据：
- MIT / BSD-3-Clause / Unlicense / Apache-2.0 → GPL-3.0 兼容（GPLv3 §与 FSF 列表）；
- GPL-3.0 → 同协议直接合并；
- MPL-2.0 → 文件级 copyleft，GPLv3 兼容（MPL-2.0 §3.2）：**本项目中源自
  MandelbrotOS 的文件继续按 MPL-2.0 条款提供**，见各文件头。

---

## 1. CoolPotOS — MIT
- 上游: https://github.com/plos-clan/CoolPotOS
- 版权: Copyright (c) 2024-2025 plos-clan
- 引入文件: `lib/rbtree.c`, `include/stitch/rbtree.h`（取自 `src/util/rbtree.c`, `src/include/rbtree.c`）
- 原始协议文本要求: 保留版权声明与许可文本 → 已在各移植文件头部保留。

## 2. SpiritFoxOS — GPL-3.0
- 上游: https://github.com/fwqfzqaz/SpiritFoxOS
- 引入文件: `kernel/mm/slab.c`（取自 `kernel/src/mm/slab.c`）
- 说明: 与本项目同为 GPL-3.0；spinlock/日志依赖替换已在文件头 "Changes" 注明。

## 3. Cinux — MIT
- 上游: https://github.com/Awesome-Embedded-Learning-Studio/Cinux
- 版权: Copyright (c) 2026 Charliechen114514
- 引入文件: `boot/boot.S`（64 位启动段结构）, `arch/x86_64/context_switch.S`
  （取自 `kernel/arch/x86_64/boot.S`, `kernel/arch/x86_64/context_switch.S`）

## 4. Phobos — GPL-3.0
- 上游: https://github.com/vagran/phobos
- 版权: Copyright (c) 2011, Artyom Lebedev <artyom.lebedev@gmail.com>
  （原文件声明 "All rights reserved. See COPYING file for copyright details."，
  COPYING 为 GPL-3.0）
- 引入文件: `lib/phobos_queue.h`（取自 `mainline/kernel/sys/queue.h` 的纯 C LIST_* 段）

## 5. MOOS — Unlicense（公有领域），部分文件 MIT
- 上游: https://github.com/nifanfa/MOOS
- 版权: Copyright (c) nifanfa（部分文件标 "licensed under the MIT licence"）
- 引入: `arch/x86_64/isr_stubs.S`（原 `NativeLib/interrupts_asm.asm`, MASM→GAS 重写）、
  `kernel/interrupts/intr.c`（原 `Kernel/Misc/IDT.cs` 分发设计 + `Misc/Interrupts.cs` 注册表，C 语言重写）
- 说明: Unlicense 无任何附加义务；MIT 义务为保留版权声明 → 已保留。
- **未引入**: 仓库内 Doom/ 目录（id Software 非商业许可，与本项目无关，明确排除）；
  ChaN FatFs、lodepng 等第三方未引入。

## 6. MoeOS — GPL-3.0
- 上游: https://github.com/KernelErr/MoeOS
- 引入: `kernel/mm/pmm.c`（取自 `src/mem/page.rs`，Rust→C 逻辑 1:1 移植）
- 说明: 原仓库声明参考 rCore / "Adventures of OS"，协议 GPL-3.0 同源。

## 7. MandelbrotOS — MPL-2.0（文件级 copyleft）
- 上游: https://github.com/petrvelicka/MandelbrotOS
- 引入文件: `kernel/drivers/mb_kbd.c`（原 `src/kernel/kbd.c`）、
  `kernel/drivers/mb_pit.c`（原 `src/kernel/pit.c`）
- **协议义务**: 按 MPL-2.0 §3.2，上述文件**继续以 MPL-2.0 条款**分发；
  修改这些文件需向本仓库贡献者开放对应源码。其余本项目代码不受影响。
- 注意: `src/include/multiboot.h` 自带 FSF/GRUB 版权头（未整体引入）。

## 8. HanOS — MIT
- 上游: https://github.com/jjwang/HanOS
- 版权: Copyright (c) 2022 JW, HanOS developer
- 引入文件: `arch/x86_64/acpi.c`, `arch/x86_64/madt.c`, `arch/x86_64/hpet.c`
  （取自 `kernel/arch/x64/` 同名文件）

## 9. Helo OS — GPL-3.0（含 Haribote 谱系代码）
- 上游: https://github.com/pzks/HeloOS , https://github.com/pzks/HeloOS4.1
- 版权: 仓库 LICENSE = GPL-3.0（含 "STON PZK 2019" 自制声明）
- 引入: `kernel/ipc/fifo.c` —— 设计借鉴 `HeloOS/Helo OS 2019.0.0.2/haribote/fifo.c`
  的 fifo32 接口语义与环形覆盖算法，**为规避上游谱系不确定风险已用干净 C 重写**，
  非逐行复制。
- ⚠ 谱系提示: Helo OS 底稿源自《30日でできる！OS自作入門》Haribote OS 示例代码，
  原书代码的授权状态在上游仓库被标为 GPL-3.0，但严格追溯存在模糊性。本项目采取
  "接口借鉴 + 重写实现"的最小风险做法；如后续被上游权利人提出异议，该文件（约
  100 行、功能单一）可轻松替换。
- HeloOS4.1 仓库无内核源码，其 `bim2hel` 工具仅作参考记录于 docs/provenance.md。

## 10. XJ380 — Apache-2.0
- 上游: https://github.com/xingji-studio/OpenXJ380
- 版权: Copyright (c) XINGJI Studios 2017-2026
- 引入: `kernel/vfs/vfs.c`（接口设计源自 `include/fs/vfs/vfs.h` + `driver/fs/vfs/vfs.cpp`
  的 vfs_callback/vfs_node 核心，C 化并裁剪 pty/procfs/socket 部分）
- Apache-2.0 义务: 保留版权/许可声明与 NOTICE（上游 LICENSES.md 列含 mikanos-hankaku
  等第三方）；本项目未引入其字体/镜像数据。

## 11. NAOS — BSD-3-Clause
- 上游: https://github.com/kadds/NaOS
- 版权: Copyright (c) 2019-2020 Kadds
- 引入: `arch/x86_64/ioapic.c`（取自 `naos/src/kernel/arch/io_apic.cc`，C++→C）
- 第三条（禁背书）适用: 本项目不暗示 NAOS 作者背书。

## 12. PlantOS — MIT
- 上游: https://github.com/plos-clan/Plant-OS
- 版权: Copyright (c) 2024 plos-clan；部分文件 "This code is released under the MIT License"
- 引入: `arch/x86_64/serial.c`（取自 `src/kernel/drivers/general/serial.c`）

## 13. Lemis — GPL-3.0
- 上游: https://gitee.com/zx909i9zx/lemis
- 版权: Copyright (C) 2026 lemon（声明见上游 `src/main.c` 文件头）
- 引入: `kernel/mm/heap.c`（取自 `0.5v+0.2.1v/src/system/kernel/other/heap.c`，
  32 位指针算术→64 位、4→16 字节对齐）

## 14. VimtuOS — GPL-3.0
- 上游: https://github.com/Fanqi89/VimtuOS
- 引入: `arch/x86_64/gdt_idt.c`（取自 `kernel/x86_64.cpp` 的 GDT/TSS/IDT 构建）、
  `include/stitch/io.h`（取自 `kernel/port.h` 的零依赖端口 I/O 内联汇编）
- 上游 kernel/ 源文件无逐文件版权头，GPL-3.0 以仓库 LICENSE 为准，已在移植头注明。

## 15. Helo OS 4.1（工具仓库，GPL-3.0）
- 上游: https://github.com/pzks/HeloOS4.1
- 说明: 该仓库仅含 `bim2hel` 镜像转换工具，无内核源码；本项目未编入其二进制，
  记录于 docs/provenance.md 以保持 15 仓库全覆盖的溯源记录。

---

## 本项目原创部分
- `boot/linker.ld`, `kernel/main/*`, `lib/kprintf.c`, `lib/string.c`,
  `include/stitch/{types,console,selftest,string}.h`, `kernel/sched/*`（胶水）、
  各测试与脚本 —— 均按 GPL-3.0 发布。
- `include/stitch/io.h` 之于 VimtuOS `port.h`、`boot/boot.S` 之于 Cinux 启动段：
  已在各文件 "Ported from" 块中逐文件记录。

## Limine Bootloader — BSD-2-Clause
- 上游: https://github.com/limine-bootloader/limine (v12.9.3)
- 版权: Copyright (C) 2019-2026 Mintsuki and contributors
- 二进制文件: `tools/limine/limine-binary/limine-bios.sys`, `limine-bios-cd.bin`, `limine-uefi-cd.bin`, `BOOTX64.EFI`
- 协议: BSD-2-Clause（见 `tools/limine/limine-binary/LICENSE`）
- 说明: 作为独立引导加载程序聚合在 ISO 上，与 GPL-3.0 内核分离分发。
