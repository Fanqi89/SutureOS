# 上游许可证核对（2026-11-02）

本文件记录 SutureOS 每个上游模块的**实际许可证**，核对方式为直接读取
`C:\Users\fanqi\Desktop\自研操作系统\<项目>\LICENSE`（或 `COPYING`），不以"听说"为准。
发布前必须以本表为准修改 README / 代码头注释；发现不一致时**以本地 LICENSE 文件为准**。

## 1. 已核实（文件实读）

| 上游项目 | 证据文件 | 实际许可证 | 与 SutureOS(GPL-3.0) 兼容 | 备注 |
|---|---|---|---|---|
| CoolPotOS | `LICENSE` | **MIT** | ✅ | README 正确；`docs/provenance.md` 旧表曾误写 GPL-3.0 |
| Cinux | `LICENSE` | **MIT**（Copyright (c) 2026 Charliechen114514） | ✅ | e1000 / HAL / NVMe / VirtIO-blk 来源 |
| XJ380 | `LICENSE` | **Apache-2.0** | ✅ | README 正确；`provenance.md` 旧表曾误写 GPL-3.0 |
| NAOS | `LICENSE` | **BSD-3-Clause**（Kadds, 2019-2020） | ✅ | README 正确；`provenance.md` 旧表曾误写 MIT |
| HanOS | `LICENSE` | **MIT** | ✅ | ACPI/MADT/LAPIC |
| PlantOS | `LICENSE` | **MIT** | ✅ | 帧缓冲/字体；本次 FAT32 LFN 参考 |
| MandelbrotOS | `LICENSE` | **MPL-2.0** | ✅（文件级 copyleft，`mb_pit.*` 保留 MPL） | |
| MOOS | `LICENSE` | **Unlicense**（public domain） | ✅ | 内存分配器 |
| MoeOS | `COPYING` 等价 `LICENSE` | **GPL-3.0** | ✅ | 任务调度/PMM |
| Lemis | `LICENSE` | **GPL-3.0** | ✅ | 堆分配器 |
| HeloOS | `LICENSE` | **GPL-3.0** | ✅ | IDT/GDT |
| VimtuOS | `LICENSE` | **GPL-3.0** | ✅ | GUI/EL0/VimtuFS2 |
| Phobos | `atomic/COPYING`、`mainline/COPYING` | **GPL-3.0** | ✅ | **根目录没有 LICENSE**，只在子目录里；`provenance.md` 旧表曾误写 MIT |
| Uinxed-Kernel | `LICENSE` | **Apache-2.0** | ✅ | v0.16.0 起：ATAPI 光驱驱动参考 |
| HBOS | `LICENSE` | **GPL-3.0** | ✅ | v0.16.0 起：AC'97 声卡参考 |

## 2. 有问题的来源（必须处理，不能假装合规）

### 2.1 SpiritFoxOS：仓库里没有任何许可证

- 实读结果：根目录无 `LICENSE` / `COPYING`；全仓库 `grep -i "GPL|General Public License|SPDX"`
  只命中 `vendor/monocypher/*`（BSD-2-Clause OR CC0-1.0，第三方 vendor 代码）。
- SutureOS 现状：`kernel/net/net.c`、`net.h`、`tcp.c`、`tcp.h`、`kernel/drivers/rtl8139.c`、
  `kernel/fs/fatfs.c`、`kernel/drivers/block.c` 的头注释都写着
  "Ported from SpiritFoxOS ... GPL-3.0"。
- **结论**：这个出处依据不足。没有许可证的公开代码，默认**不授予再分发权**。
- 处置（本版采取）：
  1. 新增代码不再从该仓库拷代码（本次 FAT32 LFN 只沿用其"校验和 + 别名回退"的算法结论，
     实现是在 `fatfs.c` 里重写的）。
  2. 已登记为待确认项：**联系作者确认许可证，或把这几个模块替换为其它已授权实现**
     （rtl8139 可换 VimtuOS/Cinux 的实现，TCP 可换 lwIP 血统实现）。
  3. 发布说明与 README 都如实写出这一点，不宣称"全部上游许可已核对无误"。

### 2.2 GPL-2.0-only 项目：只能参考算法，不能拷代码

`DragonOS`（GPL-2.0）、`Utopia/Utopia-2024`（GPL-2.0）、`Reedx-Kernel`（GPL-2.0）、
`hudos`（README 声明 GPLv2）与 GPL-3.0 汇总**不兼容**（GPLv2-only 不能并入 GPLv3 作品）。
它们只能作为阅读材料；如需其能力，必须从 MIT/Apache/BSD/GPL-3 的同类实现移植或重写。

### 2.3 AGPL-3.0 项目

`Nilix`、`UniOS` 使用 AGPL-3.0。GPLv3 §13 允许与 AGPLv3 组成组合作品，但整包会带上
"网络使用也须提供源码"的义务，与当前 GPL-3.0 单纯分发不一致，**暂不引入**。

### 2.4 无许可证 / 许可证不明的其它仓库

`KunOS`、`QNOS`、`C-UnknownOS`、`GrapeOS-Course`、`aether-naos` 本地都没有 LICENSE 文件。
一律按"只能看，不能拷"处理。

## 3. 已确认不可用作移植来源的仓库

| 仓库 | 原因 |
|---|---|
| ReliefOS | `LICENSE` 是 Apache-2.0，但 `kernel/` 是未检出的空子模块（`kernel/reliefnt`），本地没有任何驱动源码 |
| Mini-OS / HanOS | 没有 LFN 实现（`fs.c` / `fat32_srv.c` 里 `LFN|0x0F|long name` 零命中） |
| DragonOS / Utopia / Reedx-Kernel / hudos | GPL-2.0-only，与 GPL-3.0 不兼容 |
| Nilix / UniOS | AGPL-3.0，义务范围超出本项目当前分发模型 |

## 4. 本轮（v0.15.0-alpha）新增代码的许可结论

| 新增文件/改动 | 来源 | 许可 | 结论 |
|---|---|---|---|
| `kernel/drivers/e1000.c/.h` | Cinux `kernel/driver/net/e1000.cpp` | MIT | ✅ 可融合；头注释已改为 MIT 并署名 |
| `kernel/net/net.c` 链路层分发 | 本仓库自写 | GPL-3.0 | ✅ |
| `kernel/fs/fatfs.c` LFN 读侧 | PlantOS(MIT) 的 ChaN FatFs 血统 + SpiritFoxOS 算法结论 | MIT + 待确认 | ⚠️ 见 §2.1；实现为在 `fatfs.c` 内重写 |
| `scripts/nic_acceptance.py`、`scripts/gui_appearance_check.py` | 本仓库自写 | GPL-3.0 | ✅ |
| `kernel/drivers/atapi.c/.h` | Uinxed-Kernel `drivers/block/ata/pata/atapi.c` | Apache-2.0 | ✅ v0.16.0 |
| `kernel/fs/iso9660.c/.h`、`iso9660_vfs.c` | CoolPotOS `module/iso9660`（参考） | MIT | ✅ v0.16.0 |
| `kernel/drivers/ac97.c/.h` | HBOS `src/ac97.c` | GPL-3.0 | ✅ v0.16.0 |
| `kernel/fs/fatfs.c` LFN 写侧 | PlantOS ChaN FatFs `put_lfn()` 规则 | MIT | ✅ v0.16.0 |
| `kernel/fs/ext2.c` 写路径 | XJ380 / Deshab-OS 的 ext4 实现（**算法参考，未拷代码**） | Apache-2.0 | ✅ v0.17.0 |
| `kernel/gui/img_bmp.c` | HBOS `src/bmp.c`（算法参考重写） | GPL-3.0 | ✅ v0.18.0 |
| `kernel/gui/img_qoi.c` | PlantOS `src/plic/qoi.c`（算法参考重写） | MIT | ✅ v0.18.0 |
| `user/apps/r3*.c` | 自研；argv 帧/目录 fd 约定参考 VimtuOS | GPL-3.0 | ✅ v0.18.0 |
| `kernel/term/cjk16.c` 字体子集 | HBOS `fonts/ZhengGeDianHei-16.ttf`（name 表 nameID 13 实读为 **SIL OFL-1.1**；Copyright 2026 yzdnn / Fontworks DotGothic16） | OFL-1.1 | ✅ v0.20.0（OFL 全文在 docs/licenses/OFL-1.1.txt） |
| `kernel/drivers/usb_ehci.c` | HanOS EHCI 驱动（MIT）为骨架；HBOS `src/usb_msc.c`（GPL-3.0）的 CBW/CSW 布局与 BOT 流程作参考 | MIT + GPL-3.0 | ✅ v0.23.0 |
| `kernel/drivers/hda.c` | PlantOS（MIT）`src/kernel/drivers/sound/hda.c` 为骨架；ReliefOS HDA 设计/实战文档仅作只读参考（未拷代码） | MIT | ✅ v0.27.0 |
| `kernel/drivers/sb16.c/.h`（域 4 SB16+DMA） | Neumann（GPL-3.0）、Powerint-DOS-386（GPL-3.0）**仅核对寄存器顺序，未拷代码** | GPL-3.0 | ✅ v0.57.0，见 THIRD_PARTY_LICENSES.md §22 |
| `kernel/drivers/usb_hid.c/.h` + EHCI HID hooks（域 8） | 自写；依据 USB HID 1.11 / USB 2.0 / EHCI 规范，传输层复用既有 usb_ehci（HanOS MIT + HBOS GPL-3.0 已登记） | GPL-3.0 | ✅ v0.57.0，见 THIRD_PARTY_LICENSES.md §24 |
| `kernel/drivers/virtio_gpu.c/.h`（域 9 virtio-gpu 2D） | 自写；依据 OASIS VirtIO 1.2 规范 + 仓内 virtio_blk.c 参考 | GPL-3.0 | ✅ v0.57.0，见 THIRD_PARTY_LICENSES.md §23 |
| `kernel/term/flanterm/`（终端渲染器） | flanterm（**BSD-2-Clause**，mintsuki；vendored 自 CoolPotOS），改动标注 `SutureOS fix` | BSD-2 | ✅ 见 THIRD_PARTY_LICENSES.md §33 |
| `kernel/syscall/dynlink.c`（内核动态链接器） | 参考 musl `src/ldso/dynlink.c`（**MIT**），自写实现、未拷代码 | MIT（reference） | ✅ 见 THIRD_PARTY_LICENSES.md §34 |
| `kernel/drivers/atapi.c/.h`（域 5 ATAPI 光驱） | Uinxed-Kernel（**Apache-2.0**，`LICENSE` 实读，`NOTICE` 已核对）；协议流程重写 | Apache-2.0 | ✅ v0.16.0，见 THIRD_PARTY_LICENSES.md §31 |
| ext2 写算法 / 内存回收思路（参考） | Deshab-OS / Lnxrm（**Apache-2.0**，仅算法对照、未拷代码） | Apache-2.0（reference） | ✅ 登记 THIRD_PARTY_LICENSES.md §32、docs/provenance.md |
| `kernel/fs/mayfs.c/.h`（域 6 MayFS 可写） | MayOS `kernel/fs.c`，**实读 LICENSE = MIT**（清单误记 GPL-3.0，以文件为准）；实现自写 | 上游 MIT / 本项目 GPL-3.0 | ✅ v0.57.0，见 THIRD_PARTY_LICENSES.md §30 |
| `kernel/fs/os67fs.c/.h`（域 6 OS67 inode FS 只读） | 规格源自 OS67（GPL-3.0），按规格重写、未拷代码 | GPL-3.0 | ✅ v0.57.0，见 THIRD_PARTY_LICENSES.md §29 |
| `kernel/fs/ext4.c/.h`（域 6 ext4 只读） | 自写；依据 Linux 公开 ext4 规格 | GPL-3.0 | ✅ v0.57.0，见 THIRD_PARTY_LICENSES.md §26 |
| `kernel/fs/vimtufs2.c/.h`（域 6 VimtuFS2 只读） | 规格源自 VimtuOS（**GPL-3.0**），按规格重写、未拷代码 | GPL-3.0 | ✅ v0.57.0，见 THIRD_PARTY_LICENSES.md §27 |
| `kernel/drivers/vmsvga.c/.h`（域 9 vmsvga 2D） | 自写；VMware SVGA II 公开规格 + QEMU 实测 | GPL-3.0 | ✅ v0.57.0，见 THIRD_PARTY_LICENSES.md §28 |
| `kernel/fs/sysfs_vfs.c`（域 6 合成 /sys） | LunaixOS sysfs 布局 + Linux /sys 约定，**仅机制、未拷代码** | GPL-3.0 | ✅ v0.57.0，见 THIRD_PARTY_LICENSES.md §25 |
| `kernel/fs/procfs_vfs.c`（域 6 合成 /proc） | XJ380（Apache-2.0）procfs 节点表思路、LunaixOS procfs 布局、Linux /proc 文本约定，**仅机制、未拷代码** | Apache-2.0/MIT（机制）+ GPL-3.0（实现） | ✅ v0.57.0，见 THIRD_PARTY_LICENSES.md §21 |
| `kernel/time/time_source.c/.h`（域 11 跨架构时基） | LoongOS（MIT，AArch64）、token-os（MIT，aarch64）、MikuOS（RISC-V 参考）**仅作机制蓝本，未拷代码** | MIT（机制）+ GPL-3.0（实现） | ✅ v0.57.0，见 THIRD_PARTY_LICENSES.md §20 |
| v0.57.0 批次（HPET/LAPIC/intr/loader 页池/HDA 音量/FAT32 sync/自写引导链） | HanOS hpet.c（GPL-3.0）骨架；PlantOS hda.c（MIT）骨架；e1ruBSD/XEKernelOS/zeroAOS/SukiOS/myos/AntzOS 六份引导机制蓝本（BSD-2/MIT/Apache-2.0/GPL-3.0/BSD-2/MIT）；其余为本仓库自写 | GPL-3.0 / MIT / BSD-2 / Apache-2.0（聚合兼容） | ✅ 见 docs/porting-domains-report.md 与 THIRD_PARTY_LICENSES.md §19 |
| `boot/selfboot/stage1.S` / `stage2.S` / `stage2.ld`、`scripts/build_boot_disk.py`、`scripts/selfboot_acceptance.py` | e1ruBSD `bootsect.S`+`stage2.S`（BSD-2）、XEKernelOS `boot/stage2.asm`（MIT）、zeroAOS `v0.10-float/boot.asm`（Apache-2.0，仅 VBE 机制）、SukiOS `boot/boot.S`（GPL-3.0，长模式顺序）、myos `boot.asm`（BSD-2）、AntzOS `boot/mbr.asm`（MIT）；**全部读后以 GAS 重写，未拷可识别代码块** | BSD-2 / MIT / Apache-2.0 / GPL-3.0 / BSD-2 | ✅ v0.57.0（逐条登记见 THIRD_PARTY_LICENSES.md §19） |

## 5. 维护规则

1. 每次移植前先 `read` 上游 `LICENSE`/`COPYING`，把结果写进本文件与
   `docs/provenance.md`，再动代码。
2. 头注释必须写：上游项目、上游文件路径、**实际许可证**、改动摘要。
3. 上游没有许可证 → 只能看算法、不能拷代码；已在树里的历史拷贝必须登记在 §2.1。
4. GPL-2.0-only 与 AGPL-3.0 项目不进代码树。

## 6. v0.70 外部贡献（待补许可）

- **ConlyesOS 作者**：virtio-gpu 帧缓冲模式 + 图标重绘，2026-11-25 以整棵工作树形式交付。
  交付物无许可声明 → 列为「作者授权使用、条款待书面确认」，项目按 GPL-3.0 收录并署名。
  在其许可明确之前，不得把这份代码当作可向第三方以宽松许可再分发的素材。
