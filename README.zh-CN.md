# SutureOS（缝合怪系统）

[English](README.md) | **简体中文**

[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-x86__64-lightgrey.svg)]()
[![Status](https://img.shields.io/badge/status-alpha-orange.svg)]()

SutureOS 是一个实验性的 **x86_64 单内核操作系统**，通过移植并整合多个开源爱好者操作系统
的成熟组件构建而成。项目不会从零重写每个子系统，而是把经过验证的驱动、文件系统、协议栈和
算法组合起来，用少量原创的适配/胶合代码连接，并在真实模拟器上逐项验证。

"缝合怪"这个名字描述的正是这种开发方式，也对应项目的核心原则：
**可以移植功能，不能移植外观。** 启动呈现与整套 GUI 视觉层（任务栏、窗口边框、标题栏、
配色、字体、指针、开始菜单）由 SutureOS 自绘，并刻意保持冻结。

| | |
|---|---|
| 最新发布 | **v0.57.0-alpha**（十二域移植收尾） |
| 上一版本 | **v0.56.0-alpha**（Wayland 前置能力从 ring 3 跑通） |
| 架构 | x86_64，Limine 引导协议（BIOS） |
| 语言构成 | C + x86_64 汇编，含一个 Rust `no_std` 组件 |
| 许可证 | **GPL-3.0**（上游组件分别使用 MIT / Apache-2.0 / BSD-2 / BSD-3 / MPL-2.0 / Unlicense / GPL-3.0 / OFL-1.1） |

---

## 概述

- **移植优先。** 移植版图覆盖 **64 个候选开源操作系统项目**（分属十二个能力域）。
  当前源码树实际使用其中 **32 个**：16 个直接移植（驱动、文件系统、调度器、协议栈、引导代码
  在实际使用），16 个提供磁盘格式、算法或机制蓝本并按规格重写；其余 32 个候选分别由已选实现
  等价覆盖、待许可复核，或作为后续批次的机制蓝本保留。
- **界面原创。** 图形外壳完全由 SutureOS 绘制，不引入任何上游桌面环境、合成器或皮肤。
- **以验证为准。** 下列每项能力都在 QEMU 中运行过，并在宿主侧独立核对（解析镜像、分析 WAV、
  逐像素比对、SHA-256 比对）。
- **可复现构建。** 使用 Python 构建脚本与仓库内的交叉工具链，从干净源码树生成内核和可引导 ISO。

---

## 项目状态

**已发布：** `v0.57.0-alpha` —— 十二域移植收尾。

**本次发布的主要新增：**

- 自写 64 位 BIOS 引导链（MBR → stage2 → long mode），与 Limine 共用同一个 `kernel.elf`
- 真实 HPET、用 HPET 校准的 LAPIC 定时器，以及通用时基 `time_source`
- HDA 与 AC'97 输出音量控制；Sound Blaster 16 + 8237 DMA 播放
- 合成 `/proc` 与 `/sys`
- USB HID 键盘/鼠标与热插拔重扫
- virtio-gpu 2D 与 VMware SVGA II（vmsvga）2D 后端（显式 opt-in）
- DHCP 客户端与 DNS 解析器
- 只读 ext4、VimtuFS2、OS67（Minix v1）文件系统；可写 MayFS

当前源码树构建产物：`kernel.elf` 约 3.76 MB，`sutureos.iso` 约 17.6 MB；开机自检 **16/16**，
全局符号 **1381** 个、**0 冲突**。

---

## 功能特性

**引导与平台**
- 基于 Limine 的 BIOS 引导（ISO 内含 UEFI El Torito 镜像，但尚未验证 UEFI 路径）
- 原创 64 位 BIOS 引导链：512 字节 MBR → stage2（16/32 位 → long mode）
- 安装到硬盘、覆盖式系统升级、用户数据持久化

**CPU、中断与时间**
- GDT/IDT、PIC 重映射、IOAPIC（ExtINT 级联）、LAPIC 定时器、SMP 启动
- 真实 HPET、HPET 校准 LAPIC、8254 PIT 回退、通用单调时基

**内存与进程管理**
- 物理页分配器、内核堆、Slab 分配器、四级页表
- ring-3 用户态、每进程地址空间、`execve`、libc、ELF 加载器
- pipe、`AF_UNIX` socketpair、`poll(2)`、`memfd` + `MAP_SHARED`、`SCM_RIGHTS` 描述符传递

**存储与文件系统**
- ATA PIO、AHCI/SATA、NVMe、virtio-blk、ATAPI 光驱、USB 大容量存储
- ramfs、FAT32（VFAT 长名、多卷）、ext2（可读写）、SquashFS、ISO9660
- ext4（只读）、VimtuFS2（只读）、OS67 Minix-v1（只读）、MayFS（可读写）
- 合成 `/proc` 与 `/sys`

**网络**
- RTL8139 与 e1000 驱动、ARP、IPv4、ICMP、UDP、TCP（主动/被动打开、重传）
- DHCP 客户端、DNS 解析器、HTTP/1.0 `wget`

**USB 与输入**
- EHCI 主机控制器、USB 大容量存储（BOT）、USB HID boot 键盘/鼠标与热插拔
- PS/2 键盘与鼠标

**图形、音频与 GUI**
- 线性帧缓冲终端（flanterm），支持 GB2312 中文字形
- virtio-gpu 2D 与 VMware SVGA II 2D 后端
- AC'97、Intel HDA、SB16 音频输出与音量控制
- 原创 GUI：任务栏、开始菜单、窗口管理、六个应用
- Rust `no_std` 渲染器原型（`guislint`、`guishell`）：圆角、亚克力模糊、过渡动画、实时时钟

---

## 架构

```
+-------------------------------------------------------------+
| Ring 3                                                       |
|   用户 libc   |  ring-3 工具   |  Wayland 探针（wlprobe）     |
+------------------------------+------------------------------+
| Ring 0                       |                              |
|   系统调用层                 |  VFS                         |
|   调度器（EEVDF）            |  文件系统                    |
|   TCP/IP + DHCP/DNS          |  GUI 合成器 + 应用           |
+------------------------------+------------------------------+
| HAL                                                          |
|   块设备层 | 网卡 | 帧缓冲/GPU | 输入 | 音频                   |
+-------------------------------------------------------------+
| x86_64：CPU/APIC/ACPI/PCI/内存/存储/网络/显示/输入           |
+-------------------------------------------------------------+
```

仓库结构（节选）：

```
boot/          Limine 配置、链接脚本、自写 BIOS 引导链
arch/x86_64/   中断、MMU、调度、系统调用、ACPI/HPET/LAPIC
kernel/
  drivers/     块设备、网络、USB、音频、帧缓冲、virtio-gpu、vmsvga、SB16
  fs/          ramfs 与十个文件系统驱动
  gui/         原创桌面外壳、应用、图片解码
  ipc/         pipe、AF_UNIX socket、memfd、SCM_RIGHTS
  net/         链路层、ARP/IPv4/ICMP/UDP/TCP、DHCP、DNS
  shell/       交互式 shell（87 条命令）
  syscall/     系统调用分发、ELF 加载器、ring-3 切换
  time/        通用时基注册表
  install/     安装器、升级、持久化
  vfs/         统一 VFS 与 ramfs
lib/ include/  内核库与公共头文件
user/          ring-3 libc 与工具
rust/          no_std 系统层与软件光栅化器原型
scripts/       构建脚本与宿主侧验收测试
tools/         交叉工具链与 Limine 二进制
docs/          移植、许可、安装、发布与路线图文档
tests/         符号扫描、SquashFS 宿主 harness
```

---

## 构建

### 环境要求

- **clang**（Windows 上为 MSYS2 `mingw-w64-x86_64-clang`），目标 `x86_64-unknown-none-elf`
- **x86_64-elf-ld** 与 **x86_64-elf-objcopy**（`tools/cross/bin`）
- **NASM**、**xorriso**、**Python 3**
- 可选：**Rust**（`cargo`，`x86_64-unknown-none` target），用于 Rust 系统层

### 构建命令

```bash
python3 scripts/build.py           # 内核      -> build/kernel.elf
python3 scripts/build_iso.py       # 可引导 ISO -> build/sutureos.iso
python3 scripts/build_boot_disk.py # 可选：自写 BIOS 引导链镜像
```

版本号唯一来源是 `include/sutureos/version.h`。发布顺序固定为：改版本号 → 构建 → 构建 ISO →
运行验收 → 发布。

---

## 运行

### QEMU（BIOS）

```bash
qemu-system-x86_64 \
  -machine pc -m 256 -vga std -display none \
  -serial file:build/serial.log \
  -cdrom build/sutureos.iso -boot d \
  -drive id=adisk,file=build/fat32.img,format=raw,if=ide \
  -smp 2 -no-reboot \
  -monitor tcp:127.0.0.1:45470,server,nowait
```

- `-vga std` 提供终端与 GUI 使用的线性帧缓冲。
- 用 `python3 scripts/mkfat32_testimg.py` 生成 FAT32 测试镜像。
- 读取串口日志前必须通过 monitor 的 `quit` 正常退出；直接杀进程可能导致日志尾部丢失。
- 也可使用封装脚本 `scripts/qemu_boot.ps1`（串口日志、按键注入、monitor 命令、保证干净退出）。

### VMware Workstation

`scripts/vmware_boot.ps1` 会创建一个一次性虚拟机，把 ISO 与 raw 磁盘镜像作为 SATA 设备挂载，
记录串口日志并在结束后自动关机。VMware 需要"仅描述符 + FLAT extent"的 VMDK，脚本会自动生成。

### 真机与已安装系统

1. 用 `sutureos.iso` 启动；
2. 以 root 运行 `install`；
3. 选择目标磁盘（默认不选中第一块盘），输入 `yes` 确认，可选设置 root 密码并创建普通用户；
4. 重启并从硬盘启动。

细节见 [`docs/INSTALLER.md`](docs/INSTALLER.md)，持久化见 [`docs/PERSISTENCE.md`](docs/PERSISTENCE.md)。

---

## 使用 SutureOS

启动流程固定为：滚动英文内核日志 → SutureOS 横幅 → 干净终端（`sutureos#`）。GUI 需要显式输入
`startgui` 进入。

### Shell

共 87 条命令，`help` 可查看完整列表。

| 分组 | 命令 |
|---|---|
| 文件与磁盘 | `ls` `cd` `pwd` `cat` `write` `append` `hexdump` `wc` `head` `tail` `grep` `stat` `rm` `mkdir` `rmdir` `touch` `cp` `mv` `mount` `umount` `df` `sync` |
| 系统 | `help` `uname` `free` `date` `ps` `kill` `cpus` `log` `timesrc` `clear` `reboot` `shutdown` |
| 用户 | `login` `su` `sudo` `whoami` `id` `passwd` `users` `useradd` `userdel` |
| 网络 | `ping` `dhcp` `dns` `nslookup` `wget` `tcpecho` `tcpsend` `tcpserve` `tcpapi` `tcpstat` `netstat` |
| 硬件 | `usbinfo` `kbdstat` `mouseinfo` `beep` `hdatest` `hdavol` `ac97vol` `sb16test` |
| 显示 / GUI | `startgui` `guislint` `guishell` `vga2d` `vmsvga` |
| ring 3 | `run` `ushell` `ring3` `ring3test` `unixtest` `shmtest` `fdpass` `systest` `pipetest` |

### GUI

- `startgui` 打开原创桌面，包含六个应用：Task Manager、Calculator、About SutureOS、
  Image Viewer（BMP/QOI）、Text Editor、Switch User。
- 窗口支持拖动、最小化/最大化/还原、关闭、双击标题栏最大化；四个应用固定在任务栏。
- `guislint` 渲染 Rust UI 原型的单帧画面；`guishell` 运行其可交互桌面外壳原型
  （窗口、任务栏、开始菜单、过渡动画）。

### 用户与权限

账号保存在 `/etc/passwd` 与 `/etc/shadow`，密码使用 PBKDF2-HMAC-SHA256。已实现 `su`、`login`、
`sudo` 与密码管理。需要注意：`sudo` 是策略层，所有任务运行在 ring 0，因此它不是硬件安全边界。

---

## 存储与文件系统

| 驱动 | 文件系统 | 访问方式 | 说明 |
|---|---|---|---|
| `xj_vfs` | ramfs | 读写 | 根文件系统 |
| `fatfs` | FAT32 | 读写 | VFAT 长名、多卷 |
| `ext2` | ext2 | 读写 | 直接/单级/双级间接写 |
| `ext4` | ext4 | **只读** | extent 树；不支持的特性按名称拒绝 |
| `squashfs` | SquashFS 4.0 | 只读 | 仅 gzip |
| `iso9660` | ISO9660 | 只读 | 数据光盘；El Torito 启动盘会跳过 |
| `vimtufs2` | VimtuFS2 | **只读** | v5/v4/v3 |
| `os67fs` | OS67（Minix v1） | **只读** | inode 缓存算法移植 |
| `mayfs` | MayFS | 读写 | 双槽校验和快照 |
| `procfs` | 合成 `/proc` | 只读 | meminfo、uptime、version、cpuinfo、mounts |
| `sysfs` | 合成 `/sys` | 只读 | kernel/ 与 block/ 树 |

块设备编号：ATA 0–1，AHCI 4–7，NVMe 8，内存盘 9，virtio-blk 10，ATAPI 光驱 12–15，
USB 大容量存储 16。

挂载方式：`mount <fstype> <drive> <mountpoint>`；`sync` 刷所有已挂载卷。

---

## 硬件支持

| 子系统 | 支持 | 模拟器参数 |
|---|---|---|
| 音频 | AC'97、Intel HDA、SB16 + 8237 | `-device AC97,audiodev=a0`、`-device intel-hda -device hda-duplex,audiodev=a0`、`-device sb16,audiodev=a0`（配合 `-audiodev wav,...,out.frequency=48000`） |
| 显示 | 线性帧缓冲、virtio-gpu 2D、VMware SVGA II 2D | `-vga std`、`-vga none -device virtio-gpu-pci`、`-vga none -device vmware-svga` |
| USB | EHCI、HID 键盘/鼠标、大容量存储 | `-device usb-ehci -device usb-kbd -device usb-mouse` |
| 网络 | RTL8139、e1000 | `-netdev user,id=n0 -device rtl8139,netdev=n0` |
| 存储 | ATA、AHCI、NVMe、virtio-blk、ATAPI、USB BOT | `-drive if=ide`、`-device ich9-ahci ...`、`-device nvme ...`、`-device virtio-blk-pci ...` |

virtio-gpu 与 vmsvga 后端严格 opt-in：默认启动不会初始化它们，因此终端与 GUI 的冻结呈现不受影响。

---

## 上游来源与许可

最初的移植计划枚举了 **64 个候选开源操作系统项目**（分属十二个能力域，其中 62 个许可相容）。
当前源码树实际使用其中 **32 个**：

- **16 个直接移植**：驱动、文件系统、调度器、协议栈与引导代码在实际使用（VimtuOS、Cinux、
  CoolPotOS、SpiritFoxOS、Phobos、MOOS、MoeOS、MandelbrotOS、HanOS、HeloOS、XJ380、NAOS、
  PlantOS、Lemis、Uinxed-Kernel、HBOS）。
- **16 个作为格式/算法/机制参考**，实现按其规格重写（e1ruBSD、XEKernelOS、zeroAOS、SukiOS、
  myos、AntzOS、LoongOS、token-os、MikuOS、LunaixOS、Neumann、Powerint-DOS-386、OS67、MayOS、
  Deshab-OS、Lnxrm）。

其余 **32 个候选**的去向：**8 个**由已选实现等价覆盖（同一能力域只保留一份最优实现）、
**3 个**待许可复核或 UNVERIFIED（VergeOS-Open、YiYiYa、Nilix）、**21 个**作为跨架构/应用层
机制蓝本保留给后续批次。逐来源的状态表见
[`docs/porting-source-map.md`](docs/porting-source-map.md)。

此外，还有 5 类公开规范按文本实现（Linux ext4 磁盘格式、Linux `/sys` 约定、VMware SVGA II、
OASIS VirtIO 1.2、USB HID 1.11 / USB 2.0 / EHCI 1.0），以及 4 个非 OS 组件（Limine 引导器、
flanterm 终端渲染器、musl 的 ELF 动态链接器参考、SIL OFL-1.1 中文字体）。

逐 OS 模块清单、逐文件来源与许可审计维护在：

- [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md) —— 版权声明与许可文本
- [`docs/provenance.md`](docs/provenance.md) —— 模块 → 上游 → 源文件 → 前缀映射
- [`docs/licensing-audit.md`](docs/licensing-audit.md) —— 上游仓库许可审计

**已知许可注意项：** SpiritFoxOS 是直接来源之一（slab 分配器、TCP/IP 栈、AHCI、网络架构），
但该仓库没有 `LICENSE` 文件；相关来源已记录并持续复核。

---

## 测试与质量门禁

每项能力都有宿主侧验收脚本：启动 QEMU、通过 QMP 注入真实按键，并校验串口输出、截图、WAV
或磁盘镜像。

每个移植批次都遵循三条规则：

1. QEMU 能到 shell 且无内核 panic，开机自检数量不下降（当前 **16/16**）。
2. 能力在宿主侧被独立验证（解析磁盘镜像、分析 WAV、逐像素比对、SHA-256 比对）。
3. GUI 外观门禁报告"活数据矩形之外 **0 像素**变化"（`scripts/gui_baseline_shot.py`）。

其他门禁：`tests/symbol-scan.ps1`（全局符号冲突当前为 **0**，共 1381 个符号），以及 `scripts/`
下的逐域验收脚本。

运行部分验收：

```bash
python3 scripts/hpet_acceptance.py            # 时钟、自检
python3 scripts/usb_hid_acceptance.py         # USB HID 与热插拔
python3 scripts/virtio_gpu_acceptance.py      # virtio-gpu 2D
python3 scripts/vmsvga_acceptance.py          # VMware SVGA II 2D
python3 scripts/mayfs_acceptance.py           # MayFS（需先生成夹具）
python3 scripts/gui_baseline_shot.py          # 外观门禁
```

说明：

- 文件系统测试需要对应夹具：`scripts/mkfs_*.py` / `scripts/mkfat32_testimg.py`。
  写测试会改写 FAT32 镜像，跑 GUI 测试前需重新生成。
- QMP 按键必须逐字符发送，验收脚本已封装该逻辑。
- 并行运行多个 QEMU 时，每个实例使用独立的 monitor 端口与磁盘镜像副本。

---

## 已知限制

- **UEFI 启动尚未验证**；BIOS 是已验证路径。
- 两套调度器并存（`task.c` 协作轮转与 EEVDF）；该路径上刻意避免任务上下文让出。
- 所有代码运行在 ring 0；`sudo` 是策略层而非安全边界。
- **USB HID：** 仅 high-speed；仅支持 boot 协议设备；无 LED/NKRO；热插拔在 shell 空闲循环中轮询。
- **virtio-gpu / vmsvga：** 仅 2D；上限 1024×768；无 3D、硬件光标或 vsync。
- **SB16：** 8-bit 单声道 22050 Hz，轮询完成判定。
- **AC'97 音量：** QEMU 的 WAV 后端不会把 mixer 增益写入录音，寄存器回读是证据；未在真机验证。
- **DHCP/DNS：** 仅 A 记录；无租约续期、IPv6 或 TCP 回退。
- **ext4 / VimtuFS2 / OS67FS：** 只读，且对支持的磁盘特性有明确取舍；各驱动文档中有细节。
- **MayFS：** 单文件硬上限 4095 字节（格式不变式）；每卷 48 条记录。
- **GUI 视觉重做** 刻意暂停；原创桌面保持冻结。
- 部分驱动仅在 QEMU 下验证，真机验证仍在进行。

---

## 文档

| 文档 | 内容 |
|---|---|
| [`docs/porting-domains-report.md`](docs/porting-domains-report.md) | 逐域移植报告（模块、许可、改动、验收） |
| [`docs/porting-source-map.md`](docs/porting-source-map.md) | 64 个候选来源的完整状态表 |
| [`docs/porting-final-summary.md`](docs/porting-final-summary.md) | 覆盖范围、剩余缺口、待授权项汇总 |
| [`docs/provenance.md`](docs/provenance.md) | 模块/文件级来源映射 |
| [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md) | 版权声明与许可登记 |
| [`docs/licensing-audit.md`](docs/licensing-audit.md) | 上游许可审计 |
| [`docs/INSTALLER.md`](docs/INSTALLER.md) | 安装器与磁盘布局 |
| [`docs/PERSISTENCE.md`](docs/PERSISTENCE.md) | 用户数据持久化 |
| [`docs/multiuser.md`](docs/multiuser.md) | 账号与密码语义 |
| [`docs/RELEASING.md`](docs/RELEASING.md) | 发布流程 |
| [`docs/porting-roadmap.md`](docs/porting-roadmap.md) | 移植路线图 |
| [`docs/architecture.md`](docs/architecture.md) | 架构说明 |

---

## 贡献

欢迎贡献。请遵守以下约束：

- 新增或移植的文件头必须写明 **上游项目、源文件、实读许可与改动摘要**。
- 为模块分配符号前缀，并运行 `tests/symbol-scan.ps1`（冲突必须为 0）。
- 每项能力都需要真实 QEMU 运行记录，并在宿主侧独立验证。
- 不得修改 GUI 视觉层与启动呈现；移植只能带来功能，不能带来界面资源。
- 构建与测试脚本使用 Python；补丁逻辑应写入脚本文件，避免平台相关的内联命令引号问题。
- 问题与 Pull Request 请提交到 <https://github.com/Fanqi89/SutureOS>。

## 安全性

SutureOS 是实验性爱好者操作系统，**不适合生产环境使用**。目前所有内核与用户代码都运行在
ring 0，`sudo` 是策略机制而非硬件隔离边界。请勿部署在存放敏感数据的系统上。安全问题欢迎通过
仓库 issue 或私有安全通道反馈。

---

## 许可证

SutureOS 以 **GNU General Public License v3.0** 发布（[`LICENSE`](LICENSE)）。
上游组件保留各自原始许可证，完整清单见 [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md)。

## 致谢

感谢所有上游项目的作者与贡献者：CoolPotOS、SpiritFoxOS、Cinux、Phobos、MOOS、MoeOS、
MandelbrotOS、HanOS、HeloOS、XJ380、NAOS、PlantOS、Lemis、VimtuOS、Uinxed-Kernel、HBOS、
OS67、MayOS、LunaixOS、Neumann、Powerint-DOS-386，以及来源文档中列出的规范与参考项目作者。

> *"站在巨人的肩膀上——哪怕我们拼出来的是一个缝合怪。"*
