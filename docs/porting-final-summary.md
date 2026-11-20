# SutureOS 十二域移植总说明（收口版）

基线：`v0.56.0-alpha`（`badfb4d`），本轮工作树**未提交、未打 tag、未发 Release**（按任务书第 7 条：涉及审批的动作一律跳过并记为未完成）。GUI 视觉层（任务栏/窗口边框/标题栏/配色/字体/指针/开始菜单）与启动呈现**零改动**，`scripts/gui_appearance_check.py` 动态矩形外 **0 像素**。

最终产物（本机最终内核）：`kernel.elf` **3,943,656 B**、`sutureos.iso` **18,495,488 B**；内核自检 **16/16**；`tests/symbol-scan.ps1` **1381 个全局符号、0 冲突（138 个对象）**；自写 64 位 BIOS 引导链 `scripts/selfboot_acceptance.py` **25/25**。

## 1. 十一个能力域（第 12 域见下）落地总表

| # | 能力域 | 落地内容（最优实现） | 验收证据 |
|---|---|---|---|
| 1 | 引导 / 启动 | Limine（multiboot1，BIOS+UEFI）+ **自写 64 位 BIOS 链**（MBR stage1 → stage2 16→32→long mode → `_start64`，与 Limine 共用同一 `kernel.elf`） | `selfboot_acceptance` 25/25（启动到 `sutureos#`，16/16 自检）；`hpet_acceptance` 里 `[init]` 序列不变 |
| 2 | CPU / 中断 / 时钟 | 真实 HPET、LAPIC 定时器用 HPET 校准（62.8M ticks/s）、8254 mode3 半速修正、`moos_intr` 重复计数修复、通用时基 `time_source`（HPET 100 MHz best + jiffies 回退） | `hpet_acceptance` 13/13；`time_source_acceptance` 10/10；自检 16/16 |
| 3 | 内存 / 调度 / 进程 | 用户帧池位图 + free、`sys_mmap` 失败回滚返回 `-ENOMEM`（不再映射物理页 0）、`sys_munmap` 回收帧、memfd 用 PTE bit9 区分 | 自检 16/16；`selfboot` 25/25；memfd/wlprobe 链路（v0.56 既有） |
| 4 | 声音 / 音频 | AC'97 PCM + **输出音量控制**（master 0x02 + PCM 0x18，ATTEN 1.5 dB/步）、HDA 输出 + 音量、**SB16 + 8237 DMA**（8-bit mono 22.05 kHz） | `sb16_acceptance` 23/23（WAV 440.04 Hz/0.5 FS）；`ac97_volume_acceptance` 7/7（回读 0x1515）；`hda_volume_acceptance` 9/9 |
| 5 | 存储 / 块设备 | ATA PIO、AHCI、NVMe、virtio-blk、ATAPI、USB BOT 存储；FAT32 sync 补 `blk_flush` | `usb_domain_acceptance` 7/7；`sync_acceptance` 6/6；FAT32 宿主 21/21（既有） |
| 6 | 文件系统 | ext2（读写）、ext4（**只读**，extent depth 0..4）、FAT32（读写 + LFN）、SquashFS、ISO9660、ramfs、**VimtuFS2（只读 v5/v4/v3）**、**OS67 inode FS（Minix v1 只读）**、**MayFS（可写双槽校验和快照）**、**合成 procfs/sysfs** | `ext4_acceptance` 44/44 + 宿主 217/217；`vimtufs2_acceptance` 62/62 + 宿主 173/173；`os67fs_acceptance` 14/14 + 宿主 128/128；`mayfs_acceptance` 27/27 + 宿主 42/42（recovery 13/13）；`procfs` 9/9；`sysfs` 9/9；`verify_squashfs_read` 83/83 |
| 7 | 网络 | RTL8139 + e1000、ARP/IPv4/ICMP/UDP/TCP、**DHCP 客户端 + DNS 解析**（有界超时、真实 `net_poll()` 驱动 RX） | `nic_acceptance` PASS（两卡）；`dhcpdns_acceptance` 17/17；`wget_acceptance` 8/8 |
| 8 | USB / 输入 | EHCI + BOT 存储、**USB HID 键盘/鼠标（boot 协议）+ 热插拔重扫**、PS/2 键盘鼠标 | `usb_hid_acceptance` 14/14（device_add/del 热插拔）；`usb_domain_acceptance` 7/7 |
| 9 | 图形底层 | fbcon + flanterm 终端、CJK 16×16、**virtio-gpu 2D**（modern virtio 1.0）、**vmsvga（VMware SVGA II）2D** | `virtio_gpu_acceptance` 27/27；`vmsvga_acceptance` 34/34；`cjk_terminal_acceptance` 10/10 |
| 10 | GUI 应用 / 用户态 | C 桌面（任务栏/开始菜单/图片查看器/编辑器/切换用户）、ring-3 libc + execve + 文件工具、Rust no_std GUI 渲染器（设置面板/外壳）——**视觉层自绘、外观基线未动** | `imgview_acceptance` PASS；`editor_acceptance` 5/5；`apps_taskbar_acceptance` 9/9；`gui_appearance_check` 0 px |
| 11 | 跨架构 / 跨语言机制蓝本 | 把 AArch64/RISC-V 内核「单调计数器 + 已知频率 + 最优源选择」重写成 x86_64 通用时基 `kernel/time/time_source.c`；Rust `no_std` staticlib 进内核（v0.36 起）与自写软光栅 GUI 均在 x86_64 独立落地 | `time_source_acceptance` 10/10（delta≈10 ms = 1,000,120 个 10 ns tick）；`[rust] core ... PASS` |
| 12 | 工具链 / 构建 | 只借工程做法（脚本化构建、响应文件、符号扫描、验收脚本化），**不引入第三方工具链**；`stats`/`symbol-scan`/`build_boot_disk` 等自写 | `symbol-scan` 1381 符号 0 冲突；`build.py` + `build_iso.py` + 全部验收脚本可复跑 |

## 1.5 来源版图：64 个候选 → 当前使用 32 个

移植计划（十二域）枚举了 **64 个候选开源 OS 项目**（62 个许可相容）。当前源码树与其关系：

| 状态 | 数量 | 说明 |
|---|---|---|
| 直接移植（代码/驱动/文件系统/协议栈在用） | **16** | 见 README 的「直接移植」表 |
| 参考重写（按格式/算法/机制规格重写，未拷代码） | **16** | 见 README 的「参考重写」表 |
| 由已选实现等价覆盖 | **8** | ZhaoxuOS、hsunix、KKernel、Mini-OS、xbook2、Fleurix、NovaOS、NovaOS-C |
| 待许可复核 / UNVERIFIED | **3** | VergeOS-Open（UNVERIFIED）、YiYiYa（LGPL-2.1 未引入）、Nilix（按组件分许可） |
| 后续批次机制蓝本（未采用） | **21** | 跨架构/应用层蓝本，见 `docs/porting-source-map.md` |
| **合计** | **64** | |

逐来源表格与理由：[`docs/porting-source-map.md`](porting-source-map.md)。

## 2. 仍缺 / 未做（如实）

1. **arm64 / riscv64 第二架构目标**：未新增架构目标。域 11 按落地口径是「取其机制用新写的 x86_64 代码实现同等功能」，已用 `time_source`（HPET + jiffies 抽象为通用时基）+ Rust no_std + 自写渲染器落地；真正跑 ARM/RISC-V 内核不在本任务口径内。
2. **部分清单来源未逐一移植**（同域已留最优实现，按任务书「同域只留一份最优实现」）：
   - KKernel FAT16：由 FAT32 驱动覆盖（FAT16 是 FAT32 子集路径）。
   - Fleurix minix v1：由 **OS67 inode FS**（同为 Minix v1 布局）覆盖。
   - e1ruBSD vnode/ramfs、Neumann VFS 分层、hsunix POSIX 接口、LunaixOS POSIX VFS：SutureOS 已有自己的 VFS（`kernel/vfs/vfs.c`），未整套替换（任务书禁止推倒 `fatfs.c`/`fatfs_vfs.c` 的 21/21 验收）。
   - NovaOS-C `.napp` 包：未移植（`.deb` 安装链路已在 v0.5x 完成）。
   - VimtuOS `.deb` zstd：既有 `.deb` 解包已落地（v0.5x），zstd 变体未单独做。
4. **YiYiYa LGPL-2.1 工具链**：按任务书「只借工程做法，不引入第三方工具链」，未引入。
5. **GUI 视觉大改（圆角/亚克力/Fluent 方向）**：用户明确「等我想好方向再做」，本轮未动视觉层。
6. **提交 / tag / Release / 工作区外写入**：按任务书第 7 条全部跳过，记为未完成。
7. **MayFS 单文件上限 4095 字节**：这是格式本身的硬限制（`FILE_CAP=4096` 且 `data[size]==0`），任务书里「BIG.BIN >4 KiB」在该格式下不可能；夹具改用 4095 字节最大合法文件，`>4 KiB` 的读写由 ext2/ext4/FAT32 覆盖。

## 3. 待授权清单（未拷任何代码）

按素材库 2026-11-18 审计，以下来源**未直接拷贝**、未被纳入本轮实现（与本轮范围无交集或需授权）：Utopia / Utopia-2024 / DragonOS / Reedx-Kernel / HunDunOS（GPL-2.0-only）、hudos（README 声明 GPLv2 无 LICENSE 文件）、UniOS（AGPL）、GrapeOS-Course / KunOS / QNOS / C-UnknownOS / aether-naos / SaylterOS / NeoRunST-Doom / FunsOS（无 LICENSE 或徽章不构成授权）、OpenVHCOS（自定义协议且非内核 OS）、NAOS/ReliefOS 内 busybox（GPL-2.0）。这些来源的机制如需采用，按「跨架构功能蓝本」方式用 C/ASM 独立重写（本轮的 `time_source` 即此模式），不直接拷代码。

## 4. UNVERIFIED（证据不足，未当已实现）

| 项 | 缺什么 |
|---|---|
| **VergeOS-Open**（引导域来源） | 上游仓库 404、仅 1 提交，无法取得可验证实现 → 标 UNVERIFIED，未使用其代码 |
| **vmsvga 在真 VMware Workstation** | 只在 QEMU `-device vmware-svga` 验证；真 VMware 的寄存器 I/O 顺序与 FB_START 语义未实测 |
| **ext4 与 e2fsck 交叉校验** | 本机无 e2fsprogs（MSYS2 pacman 403、WSL 无发行版），无外部参照；改用生成器自解析 121/121 + 宿主独立解析 217/217 + 驱动串口数据三向一致 |
| **USB HID / SB16 / MayFS 真机** | 只在 QEMU 模型验证，未在物理硬件/VMware 上复验 |
| **MayFS 真断电注入** | 未做硬件级 torn-write 注入；证据是写序设计 + 双槽逐字节一致 + pristine→dirty 仅 27/262144 字节 + `--recovery` 主/备槽损坏恢复 |
| **CJK 字体来源** | 使用 `ZhengGeDianHei-16.ttf`（name 表实读 SIL OFL 1.1，已登记 `docs/licenses/OFL-1.1.txt`），非 UNVERIFIED；仅 GB2312 子集 |

## 5. 复跑入口

```
py -3 scripts/build.py && py -3 scripts/build_iso.py
# 逐域验收（部分需要先造夹具）
py -3 scripts/selfboot_acceptance.py        # 1 引导
py -3 scripts/hpet_acceptance.py            # 2 时钟
py -3 scripts/sb16_acceptance.py            # 4 声音
py -3 scripts/ac97_volume_acceptance.py
py -3 scripts/ext4_acceptance.py            # 6 文件系统（先跑 mkfs_ext4_testimg.py）
py -3 scripts/vimtufs2_acceptance.py --mode primary
py -3 scripts/os67fs_acceptance.py          # 先跑 mkfs_os67_testimg.py
py -3 scripts/mayfs_acceptance.py           # 先跑 mkfs_mayfs_testimg.py
py -3 scripts/procfs_acceptance.py ; py -3 scripts/sysfs_acceptance.py
py -3 scripts/dhcpdns_acceptance.py         # 7 网络
py -3 scripts/usb_hid_acceptance.py         # 8 USB
py -3 scripts/virtio_gpu_acceptance.py      # 9 图形
py -3 scripts/vmsvga_acceptance.py
py -3 scripts/gui_baseline_shot.py          # 外观门禁（0 px 才算过）
tests/symbol-scan.ps1                       # 0 冲突
```

验收夹具卫生：跑 GUI/外观类验收前必须 `py -3 scripts/mkfat32_testimg.py` 重建并把 `build/fat32.img` 复制成 `build/fat32_ata.img`（被写测试弄脏后 FAT32 ops 自检会卡在损坏目录链）。
