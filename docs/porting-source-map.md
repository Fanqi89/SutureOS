# Porting source map — the 64 candidate sources

SutureOS started from a twelve-domain plan that enumerated **64 open-source hobby OS projects** as candidate sources (62 of them license-compatible). This document records where each candidate currently stands:

- **Direct port** — code (driver, filesystem, scheduler, protocol stack, boot code) is in active use.
- **Reference rewrite** — the project supplies an on-disk format, algorithm or mechanism blueprint;
  the implementation was written from its specification, without copying source code.
- **Covered by another port** — the capability is already provided by a different selected
  implementation, following the project rule "one best implementation per capability domain".
- **Pending / UNVERIFIED** — license review outstanding, or the upstream repository has no
  verifiable implementation.
- **Not adopted (blueprint)** — cross-architecture or application-layer blueprint kept for a
  future batch.

| Source | Domain(s) in the porting map | Status | Modules / note |
|---|---|---|---|
| HEPU | 引导 / 启动 | Not adopted (blueprint) | Cross-architecture or application-layer blueprint, reserved for a future batch |
| TextOS | 引导 / 启动 | Not adopted (blueprint) | Cross-architecture or application-layer blueprint, reserved for a future batch |
| zeroAOS | 引导 / 启动 | Reference rewrite | stage2 的 VBE 模式发现 |
| e1ruBSD | 引导 / 启动；内存 / 调度 / 进程；文件系统 | Reference rewrite | stage1/stage2 引导链形态（BSD-2） |
| NeoRunST-Bootloader | 引导 / 启动 | Not adopted (blueprint) | Cross-architecture or application-layer blueprint, reserved for a future batch |
| Cinux | 引导 / 启动；内存 / 调度 / 进程；存储 / 块设备；文件系统；网络 | Direct port | multiboot1 启动段、上下文切换、ext2/ext4 基础、NVMe、virtio-blk、ELF 加载器、e1000 网卡 |
| VimtuOS | 引导 / 启动；CPU / 中断 / 时钟；声音 / 音频；文件系统；网络；图形底层；GUI 应用 / 用户态功能 | Direct port | GDT/TSS/IDT、端口 I/O、Limine 布局、EL0 切换、ring-3 libc、VimtuFS2 格式、GUI 框架参考 |
| SukiOS | 引导 / 启动 | Reference rewrite | x86_64 长模式启动顺序（PAE→CR3→EFER→CR0.PG） |
| COFUOS | 引导 / 启动 | Not adopted (blueprint) | Cross-architecture or application-layer blueprint, reserved for a future batch |
| VergeOS-Open | 引导 / 启动 | Pending / UNVERIFIED | UNVERIFIED：上游 404、仅 1 提交，无可验证实现 |
| AntzOS | 引导 / 启动；GUI 应用 / 用户态功能 | Reference rewrite | MBR→保护模式交接 |
| XEKernelOS | 引导 / 启动 | Reference rewrite | MBR→Stage2→32 位引导交接 |
| TinyOS-V1 | 引导 / 启动 | Not adopted (blueprint) | Cross-architecture or application-layer blueprint, reserved for a future batch |
| myos | 引导 / 启动 | Reference rewrite | 纯汇编 A20/GDT 配方 |
| HeloOS | CPU / 中断 / 时钟 | Direct port | fifo32 接口语义（干净重写） |
| MandelbrotOS | CPU / 中断 / 时钟 | Direct port | PS/2 键盘、PIT 定时器（MPL-2.0 文件级保留） |
| HanOS | CPU / 中断 / 时钟；USB / 输入；图形底层 | Direct port | ACPI/MADT、HPET、LAPIC、EHCI 骨架 |
| NAOS | CPU / 中断 / 时钟；USB / 输入 | Direct port | IOAPIC |
| Phobos | CPU / 中断 / 时钟 | Direct port | 队列/LIST 原语 |
| MoeOS | 内存 / 调度 / 进程 | Direct port | 物理页管理（Rust→C 1:1） |
| Lemis | 内存 / 调度 / 进程 | Direct port | 内核堆分配器 |
| MOOS | 内存 / 调度 / 进程；图形底层；跨架构 / 跨语言的功能蓝本 | Direct port | 中断 stub 与 IDT 分发（C 重写） |
| Fleurix | 内存 / 调度 / 进程；文件系统 | Covered by another port | Minix v1 由 OS67FS 覆盖（COW 未移植） |
| Lnxrm | 内存 / 调度 / 进程；文件系统 | Reference rewrite | OOM 保护 / 内存回收思路 |
| SpiritFoxOS | 内存 / 调度 / 进程；存储 / 块设备；网络 | Direct port | slab 分配器、TCP/IP 栈、AHCI、网络架构、FAT32 LFN 算法对照（仓库无 LICENSE 文件） |
| XJ380 | 内存 / 调度 / 进程；存储 / 块设备；文件系统；网络；USB / 输入；图形底层；GUI 应用 / 用户态功能；工具链 / 构建 | Direct port | VFS 接口设计、ext4 写算法参考、procfs 节点表参考 |
| kilons | 内存 / 调度 / 进程 | Not adopted (blueprint) | Cross-architecture or application-layer blueprint, reserved for a future batch |
| LianKernel | 内存 / 调度 / 进程 | Not adopted (blueprint) | Cross-architecture or application-layer blueprint, reserved for a future batch |
| HBOS | 声音 / 音频；网络；USB / 输入；图形底层；GUI 应用 / 用户态功能 | Direct port | AC'97 声卡、BOT/USB 存储参考、BMP 解码、CJK 点阵字库素材 |
| PlantOS | 声音 / 音频；文件系统；图形底层 | Direct port | 串口、FAT32 VFAT LFN 规则（读写）、HDA 骨架、QOI 解码 |
| Neumann | 声音 / 音频；文件系统；图形底层 | Reference rewrite | SB16 / 8237 DMA 寄存器时序 |
| Powerint-DOS-386 | 声音 / 音频；GUI 应用 / 用户态功能 | Reference rewrite | SB16 / DMA 时序对照 |
| Uinxed-Kernel | 存储 / 块设备；USB / 输入；图形底层 | Direct port | ATAPI 光驱 PIO（IDENTIFY PACKET/PACKET/REQUEST SENSE） |
| Deshab-OS | 存储 / 块设备；网络；图形底层；GUI 应用 / 用户态功能 | Reference rewrite | ext2 写路径的位图/间接块算法对照 |
| CoolPotOS | 存储 / 块设备；文件系统；USB / 输入 | Direct port | 红黑树、EEVDF 调度器、SquashFS/ISO9660 参考、ext2 基础 |
| ZhaoxuOS | 存储 / 块设备 | Covered by another port | FAT32/ext4 由已移植的 fatfs/ext2/ext4 驱动覆盖 |
| MayOS | 文件系统；网络 | Reference rewrite | MayFS 双槽校验和快照格式与提交语义 |
| OS67 | 文件系统 | Reference rewrite | Minix v1 inode FS 磁盘格式（superblock/inode/位图/目录项/块链） |
| hsunix | 文件系统 | Covered by another port | POSIX VFS/系统调用接口由自有 VFS + ring-3 libc 覆盖 |
| LunaixOS | 文件系统；工具链 / 构建 | Reference rewrite | POSIX VFS / procfs / sysfs 树布局 |
| KKernel | 文件系统 | Covered by another port | FAT16 由 FAT32 驱动覆盖 |
| NovaOS-C | 文件系统；网络；GUI 应用 / 用户态功能 | Covered by another port | .napp 包机制未移植，.deb 安装链路已覆盖包管理 |
| MoQiOS | 网络；跨架构 / 跨语言的功能蓝本 | Not adopted (blueprint) | Cross-architecture or application-layer blueprint, reserved for a future batch |
| Mini-OS | 网络 | Covered by another port | HTTP 客户端由 wget 覆盖 |
| LoongOS | USB / 输入；跨架构 / 跨语言的功能蓝本 | Reference rewrite | AArch64 单调计数器/tick 抽象 |
| NovaOS | 图形底层 | Covered by another port | CJK 字库/Limine 由自有 CJK 与 Limine 覆盖 |
| xbook2 | GUI 应用 / 用户态功能 | Covered by another port | ring-3 libc 由 VimtuOS 同族 libc 覆盖 |
| GrapeOS | GUI 应用 / 用户态功能 | Not adopted (blueprint) | Cross-architecture or application-layer blueprint, reserved for a future batch |
| NeoRunST-DEV-KIT | GUI 应用 / 用户态功能 | Not adopted (blueprint) | Cross-architecture or application-layer blueprint, reserved for a future batch |
| NeoRunST-Math | GUI 应用 / 用户态功能 | Not adopted (blueprint) | Cross-architecture or application-layer blueprint, reserved for a future batch |
| NeoRunST-Lua | GUI 应用 / 用户态功能 | Not adopted (blueprint) | Cross-architecture or application-layer blueprint, reserved for a future batch |
| Asterinas | 跨架构 / 跨语言的功能蓝本 | Not adopted (blueprint) | Cross-architecture or application-layer blueprint, reserved for a future batch |
| Phoenix | 跨架构 / 跨语言的功能蓝本 | Not adopted (blueprint) | Cross-architecture or application-layer blueprint, reserved for a future batch |
| tornado-os | 跨架构 / 跨语言的功能蓝本 | Not adopted (blueprint) | Cross-architecture or application-layer blueprint, reserved for a future batch |
| NPUcore-IMPACT | 跨架构 / 跨语言的功能蓝本 | Not adopted (blueprint) | Cross-architecture or application-layer blueprint, reserved for a future batch |
| UltraOS-2021 | 跨架构 / 跨语言的功能蓝本 | Not adopted (blueprint) | Cross-architecture or application-layer blueprint, reserved for a future batch |
| MikuOS | 跨架构 / 跨语言的功能蓝本 | Reference rewrite | RISC-V 64 时基/源选择 |
| token-os | 跨架构 / 跨语言的功能蓝本 | Reference rewrite | aarch64 裸机时基机制 |
| ViperOS | 跨架构 / 跨语言的功能蓝本 | Not adopted (blueprint) | Cross-architecture or application-layer blueprint, reserved for a future batch |
| SproutOS | 跨架构 / 跨语言的功能蓝本 | Not adopted (blueprint) | Cross-architecture or application-layer blueprint, reserved for a future batch |
| Nilix | 跨架构 / 跨语言的功能蓝本 | Pending / UNVERIFIED | 按组件分许可（含 AGPL），需逐文件判定后使用 |
| na-kernel | 跨架构 / 跨语言的功能蓝本 | Not adopted (blueprint) | Cross-architecture or application-layer blueprint, reserved for a future batch |
| YiYiYa | 工具链 / 构建 | Pending / UNVERIFIED | LGPL-2.1：按项目规则未引入第三方工具链 |
| ReliefOS | 工具链 / 构建 | Not adopted (blueprint) | Cross-architecture or application-layer blueprint, reserved for a future batch |

## Counts

| Status | Count |
|---|---|
| Direct port | 16 |
| Reference rewrite | 16 |
| Covered by another port | 8 |
| Pending / UNVERIFIED | 3 |
| Not adopted (blueprint) | 21 |
| **Total** | **64** |

The current tree therefore derives from **32** of the 64 candidates (16 direct ports + 16 reference rewrites). The remaining 32 are 8 covered by an existing implementation, 3 pending license review or UNVERIFIED, and 21 blueprints reserved for future batches.

Per-file attribution for the used sources: [`provenance.md`](provenance.md), [`../THIRD_PARTY_LICENSES.md`](../THIRD_PARTY_LICENSES.md), [`licensing-audit.md`](licensing-audit.md).
