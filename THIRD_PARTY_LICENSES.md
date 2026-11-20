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
- 引入文件: 
  - `lib/rbtree.c`, `include/stitch/rbtree.h`（取自 `src/util/rbtree.c`, `src/include/rbtree.c`）——红黑树
  - `kernel/sched/eevdf.c`, `kernel/sched/eevdf.h`（取自 `src/task/eevdf.c`, `src/include/task/eevdf.h`）——EEVDF 调度器
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

## 后续版本新增的移植（v0.15.0 起，逐版追加）

### 16. Cinux — MIT（网卡）
- 上游文件: `kernel/driver/net/e1000.cpp` + `e1000_layout.hpp`（**MIT**，Copyright (c) 2026
  Charliechen114514；旧注释曾误写 GPL-3.0，已在 v0.15.0 改正）
- 引入文件: `kernel/drivers/e1000.c`, `kernel/drivers/e1000.h`（v0.15.0）
- 说明: 轮询式 8254x 驱动，保留上游"轮询收包必须先做一次 MMIO 读"等结论；DMA 缓冲改静态 `.bss`。

### 17. PlantOS — MIT（FAT32 长文件名规则）
- 上游文件: ChaN FatFs 血统（`src/fs/fatfs/ff.c` 的 LFN 规则与 `put_lfn()` 算法）
- 引入文件: `kernel/fs/fatfs.c` 的 VFAT 长名读写段（v0.15.0 读侧 / v0.16.0 写侧）
- 说明: 规则实现自行编写；SpiritFoxOS 仅作为"校验和 + 别名回退"的算法对照，未拷贝其代码
  （该仓库没有 LICENSE 文件，见 `docs/licensing-audit.md` §2.1）。

### 18. HBOS — GPL-3.0（声卡）
- 上游: https://github.com/HeBitOS/HBOS
- 上游文件: `src/ac97.c` + `src/ac97.h`（GPL-3.0）
- 引入文件: `kernel/drivers/ac97.c`, `kernel/drivers/ac97.h`（v0.16.0）
- 说明: PCI 探测 + NAM codec 初始化 + NABM PCM OUT 32 条 BDL 轮询播放；与本项目同协议，
  改动清单写在两个新文件头部。


### 19. e1ruBSD / XEKernelOS / zeroAOS / SukiOS / myos / AntzOS — self-written BIOS boot chain (v0.57.0)
- 引入文件: `boot/selfboot/stage1.S`（512 字节 MBR）、`boot/selfboot/stage2.S`（16 位→32 位→x86_64
  长模式加载器）、`boot/selfboot/stage2.ld`、`scripts/build_boot_disk.py`、`scripts/selfboot_acceptance.py`
- 机制来源（**读取后改写，非逐行拷贝**）:
  - e1ruBSD `sys/arch/amd64/boot/bootsect.S` + `stage2.S`（**BSD-2-Clause**，yhcmac-prog）：
    MBR 布局、INT 13h/AH=42h 结构、stage-2 链、A20/GDT/保护模式配方
  - XEKernelOS `src/boot/boot.asm` + `src/boot/stage2.asm`（**MIT**，XEKernel）：MBR→Stage2→32 位链形
  - zeroAOS `versions/v0.10-float/boot.asm`（**Apache-2.0**，Holmes-2011）：VESA VBE 模式发现与线性帧缓冲交接
  - SukiOS `boot/boot.S`（**GPL-3.0**，bil-fis）：x86_64 长模式启动顺序（PAE→CR3→EFER→CR0.PG）
  - myos `boot.asm`（**BSD-2 文本**，doujiaozh）：A20/GDT 配方
  - AntzOS `boot/mbr.asm`（**MIT**，HuangPichao）：MBR→保护模式交接模式
- 说明: 全部以 GAS（Intel 语法）重写，按 SutureOS 的磁盘布局（stage1 512 B + stage2 16 KiB +
  flat kernel at LBA 33）适配；内核镜像零改动（复用 `boot/boot.S` 的 multiboot1 契约与新增的
  `_start64` 入口）。上游仅作机制蓝本，未拷贝可识别的代码块；D 盘素材库对应目录只读。
- Apache-2.0 义务: 保留本项版权/许可声明（zeroAOS 贡献仅为 VBE 机制描述，未包含其代码）。
- 验收: `py -3 scripts/build_boot_disk.py` + `py -3 scripts/selfboot_acceptance.py`（25/25，含宿主侧
  与独立 objcopy 产物逐字节比对、patched header 与 ELF 符号表比对、QEMU 无 CD 启动到 shell）。


### 20. LoongOS / token-os / MikuOS — cross-architecture timebase blueprint (v0.57.0, domain 11)
- 引入文件: `kernel/time/time_source.c`, `kernel/time/time_source.h`, `scripts/time_source_acceptance.py`
- 机制来源（**只读理解，未拷任何代码**）:
  - LoongOS（**MIT**，AArch64 裸机：调度/FS/ELF/virtio/TCP；generic timer + tick 抽象）
  - token-os（**MIT**，aarch64 裸机内核 + 内核内 agent）
  - MikuOS（**MIT**，RISC-V 64 内核，本地 LICENSE 实读；仓库参考）
- 说明: x86_64 侧实现完全自写——HPET（固件 period）作高分辨率源、100 Hz 调度 tick 作单调回退、
  纳秒换算按"整秒 + 余数"拆分避免 `ticks*1e9` 溢出；上游仅提供"一个带已知频率的单调计数器 + 最佳源选择"
  这一跨架构机制形状。

### 21. XJ380 / LunaixOS — synthetic /proc blueprint (v0.57.0, domain 6)
- 引入文件: `kernel/fs/procfs_vfs.c`、`scripts/procfs_acceptance.py`
- 机制来源（**只读理解，未拷任何代码**）:
  - XJ380 `driver/fs/vfs/vfs.cpp`（**Apache-2.0**）：procfs 节点表/合成文件思路
  - LunaixOS POSIX-VFS procfs 布局（**MIT**，本地 LICENSE 实读；参考）
  - Linux /proc 文本约定（`key: value` 行）
- 说明: x86_64 上完全自写——按 VFS 驱动接口实现只读合成文件，文本在 open 时生成到
  per-handle 缓冲；含上一轮失败的根因修复（VFS_O_RDONLY/WRONLY/RDWR 位掩码误判）。

### 22. Neumann / Powerint-DOS-386 — SB16 + 8237 DMA register blueprint (v0.57.0, domain 4)
- 引入文件: `kernel/drivers/sb16.c`, `kernel/drivers/sb16.h`, `scripts/sb16_acceptance.py`
- 机制来源（**只读核对寄存器顺序与硬件坑，未拷代码**）:
  - Neumann（**GPL-3.0**）：`kernel/drivers/audio/sb16/sb16.c`、`kernel/drivers/dma/dma.c`
  - Powerint-DOS-386（**GPL-3.0**）：`kernel/drivers/sb16.c`、`kernel/drivers/dma/dma.c`
- 说明: SB16/8237 寄存器语义本身是公开硬件文档；驱动由 SutureOS 自写，`sb16_`/`g_sb16_` 前缀，
  8237 24 位地址限制用独立 `.sb16_dma` 链接段解决（DMA 缓冲落 0x210000）。

### 23. OASIS VirtIO 1.2 规范 — virtio-gpu 2D 后端（v0.57.0, domain 9）
- 引入文件: `kernel/drivers/virtio_gpu.c`, `kernel/drivers/virtio_gpu.h`, `scripts/virtio_gpu_acceptance.py`
- 依据: OASIS VirtIO 1.2 规范 §2.7（PCI capability）/§4.1（virtqueue）/§5.7（GPU device）；
  virtqueue 机制参考本仓库已有 `kernel/drivers/virtio_blk.c`；**未拷贝任何第三方源码**。
  QEMU 的 capability 顺序（common@+0x0 / isr@+0x1000 / device@+0x2000 / notify@+0x3000）
  为外部观测结果，代码一律解析 capability 而不写死偏移。

### 24. USB HID 1.11 / USB 2.0 / EHCI 1.0 规范 — USB HID 类驱动（v0.57.0, domain 8）
- 引入文件: `kernel/drivers/usb_hid.c`, `usb_hid.h`、`usb_ehci.c/.h` 的 HID hooks、`scripts/usb_hid_acceptance.py`
- 依据: USB HID 1.11 附录 B（boot 键盘 8 字节/鼠标 3 字节报告、SET_IDLE 0x0A、SET_PROTOCOL 0x0B）、
  USB 2.0 §9（GET_DESCRIPTOR/SET_ADDRESS/SET_CONFIGURATION）、EHCI 1.0 §3（被 NAK 的 qTD 保持 active ⇒ 超时=无数据）。
  传输层复用本仓库既有 `usb_ehci.c`（其上游 HanOS MIT + HBOS GPL-3.0 出处已在该文件头，本批未改归属）。
  **未拷贝任何第三方源码**。

### 25. LunaixOS / Linux /sys conventions — synthetic sysfs (v0.57.0, domain 6)
- 引入文件: `kernel/fs/sysfs_vfs.c`、`scripts/sysfs_acceptance.py`
- 机制来源（**只读参考，未拷代码**）: LunaixOS 的 sysfs 树布局；Linux `/sys/block/<dev>/{size,dev,ro}` 文件约定。
- 说明: 完全自写——静态树（kernel/ + block/），块设备来自本仓库 `g_blk_devices` 链表，文本在 open 时生成。

### 26. Linux ext4 on-disk spec — read-only ext4 driver (v0.57.0, domain 6)
- 引入文件: `kernel/fs/ext4.c`, `ext4.h`, `ext4_vfs.c`, `scripts/mkfs_ext4_testimg.py`, `verify_ext4_read.py`
- 依据: Linux 内核公开规格 `Documentation/filesystems/ext4`（superblock / group descriptors / inode /
  extent tree / directory entries）。仓内 `kernel/fs/ext2.c` 仅作代码"形状"参考（块层、错误码、VFS 适配布局），
  **未复制任何第三方源码**；镜像生成器与宿主校验器均为纯标准库自写。

### 27. VimtuOS VimtuFS2 format — read-only vimtufs2 driver (v0.57.0, domain 6)
- 引入文件: `kernel/fs/vimtufs2.c`, `vimtufs2.h`, `vimtufs2_vfs.c`, `scripts/mkfs_vimtufs2_testimg.py`,
  `verify_vimtufs2_read.py`, `docs/vimtufs2-port.md`
- 格式规格与参考实现: **VimtuOS** `kernel/vfs64.h`（磁盘布局 / inode 表 / 块链 / mtime 打包）、
  `kernel/vfs64.cpp`（`VFS_O_*`/`VFS_I_*` 偏移、`sb_verify`、`inode_ok`、`rmap_*`、`vfs64_format`），
  **GPL-3.0**（与本项目兼容）。本实现是**按规格重写**，未整段拷贝 VimtuOS 源码；每个偏移在 C 文件头
  注释里标了参考行号。VimtuOS 参考目录全程只读。

### 28. VMware SVGA II register spec — vmsvga 2D backend (v0.57.0, domain 9)
- 引入文件: `kernel/drivers/vmsvga.c`, `vmsvga.h`, `scripts/vmsvga_acceptance.py`
- 依据: VMware SVGA II 公开寄存器/命令编号规格；QEMU `-device vmware-svga` 的实际行为全部靠外部实测
  （QMP `info pci`/`info qtree` + 驱动把每个读回的寄存器值打进日志）。**未拷贝任何第三方源码**。

### 29. OS67 inode filesystem (Minix v1 layout) — read-only os67fs driver (v0.57.0, domain 6)
- 引入文件: `kernel/fs/os67fs.c`, `os67fs.h`, `os67fs_vfs.c`, `scripts/mkfs_os67_testimg.py`,
  `verify_os67_read.py`, `scripts/os67fs_acceptance.py`
- 格式规格与算法参考（**GPL-3.0**，本地 LICENSE 实读）: **OS67** `inc/minix.h`（superblock / d_inode / dir_entry / IBLK / IMAP_BLK /
  ZMAP_BLK）、`inc/inode.h`（inode 缓存 API，NINODE）、`fs/inode.c`（缓存/引用计数/锁/块映射）、
  `fs/bitmap.c`（位图编号）、`fs/dir.c`（目录项）。**实现自写，未拷贝 OS67 源码**；OS67 参考目录只读。

### 30. MayOS MayFS — checksummed two-slot snapshot filesystem (v0.57.0, domain 6)
- 引入文件: `kernel/fs/mayfs.c`, `mayfs.h`, `mayfs_vfs.c`, `scripts/mkfs_mayfs_testimg.py`,
  `verify_mayfs_read.py`, `scripts/mayfs_acceptance.py`, `docs/mayfs-format-notes.md`
- 来源: **MayOS** `kernel/fs.c`（14,483 B）+ `kernel/core.h`（`File` / `FS_MAX` / `FILE_CAP`）。
- **许可（实读，权威）**: `MayOS/LICENSE` = **MIT License, Copyright (c) 2024 MayOS Project**。
  素材库清单曾记为 GPL-3.0 同族，以仓库内 LICENSE 文件为准；MIT 与本项目 GPL-3.0-or-later 相容。
- 实现自写（数据结构布局、FNV-1a 校验和与双槽/generation 语义按规格），未整段拷贝 MayOS 源码。

### 31. Uinxed-Kernel — Apache-2.0（ATAPI 光驱 PIO 驱动，v0.16.0, domain 5）
- 本地素材库路径（只读参考）: `C:\Users\fanqi\Desktop\自研操作系统\Uinxed-Kernel`
- 上游许可: **Apache License 2.0**（仓库 `LICENSE` 实读）；版权行来自源文件头
  `Copyright (C) 2020 ViudiraTech, 2026 MicroFish`；仓库另有 `NOTICE` 与 `CREDITS`。
- 引入文件: `kernel/drivers/atapi.c`、`atapi.h`（参考 `drivers/block/ata/pata/atapi.c` 的
  ATAPI 协议流程：IDENTIFY PACKET 0xA1 → PACKET 0xA0 + CDB → 按设备 Byte Count 搬数据 →
  REQUEST SENSE 兜底；**按 SutureOS 风格重写**：纯轮询、无 IRQ、每步有界等待、2048 字节扇区）。
- Apache-2.0 义务: 已保留版权/许可声明；上游 `NOTICE` 内容（OVMF、Limine 等第三方组件声明）
  已核对，本项目未分发上游二进制；如未来分发包含上游代码的产物，需同时附带该 NOTICE。

### 32. Deshab-OS / Lnxrm — Apache-2.0（算法/思路参考，无代码拷贝）
- 本地素材库路径（只读参考）: `C:\Users\fanqi\Desktop\自研操作系统\Deshab-OS`、
  `C:\Users\fanqi\Desktop\自研操作系统\Lnxrm`（两者 `LICENSE` 实读均为 Apache-2.0）。
- 用途: 只作为**算法/思路对照**——ext2 写路径的位图/间接块思路参考 Deshab-OS `CODE/dsk/ext4.c`；
  OOM 保护/内存回收思路参考 Lnxrm；**未拷贝任何代码**，SutureOS 中的实现为自写。
- 义务: 本项目未分发上述上游代码，故无 Apache-2.0 文件级义务；此条目仅用于溯源记录。

### 33. flanterm — BSD-2-Clause（终端渲染器，v0.9.0 起）
- 上游: **flanterm**，Copyright (C) 2022-2025 mintsuki and contributors；BSD-2-Clause。
  本项目 vendored 自 **CoolPotOS** `src/term/flanterm`（同为 BSD-2），保留上游许可头。
- 引入文件: `kernel/term/flanterm/`（`flanterm.c`、`flanterm_backends/fb.c`、`flanterm.h` 等，
  基本原样保留）+ SutureOS 适配层 `kernel/term/term.c/.h`（BSD-2 允许修改；改动处标注
  `SutureOS fix`，如 `plot_char_*_uncanvas` 默认前景色误用 `default_bg` 的修复）。
- 义务: 保留版权声明与许可文本；本项目已在 vendored 文件头与本节登记。

### 34. musl libc — MIT（动态链接器实现参考，v0.1x 起）
- 上游: **musl**（Copyright (C) 2005-2020 Rich Felker；MIT）。
- 用途: `kernel/syscall/dynlink.c` 的 ELF 解析/符号表/重定位/依赖加载/TLS 是**参考**
  musl `src/ldso/dynlink.c` 自行实现的（内核内动态链接器，运行在 ring 0 的内核侧），
  **未引入 musl 的构建产物或源码副本**；本项目 ring-3 libc 来自 VimtuOS 同族（见 §14）。
- 义务: 未分发 musl 代码，无文件级义务；保留此溯源与 MIT 声明。

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
