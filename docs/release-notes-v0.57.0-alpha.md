# SutureOS v0.57.0-alpha

**十二域移植收尾：补齐文件系统、USB HID、虚拟显示、音频、DHCP/DNS 与自写 BIOS 引导链等能力，全部带真机 QEMU 验收。**

本版在 v0.56.0-alpha（Wayland 前置能力从 ring 3 跑通）的基础上，完成十二个能力域的移植收尾：
新增 4 个文件系统驱动、2 个虚拟显示后端、USB HID 键鼠与热插拔、DHCP/DNS、SB16 与音量控制、
自写 64 位 BIOS 引导链、真实 HPET 与通用时基，并把上游来源版图（64 个候选 OS 项目）的
逐来源状态整理成公开文档。GUI 视觉层与启动呈现保持冻结（外观门禁 0 像素）。

---

## ■ 关键修复

| 现象 | 根因 | 修复 |
|---|---|---|
| 开机自检长期 13/14（`moos_intr` FAIL） | 中断分发对异常向量重复计数一次 | 删除重复计数，自检稳定 16/16 |
| `cat /proc/*` 全部报 operation not supported | `VFS_O_RDONLY=0x1` 与 `flags & (WRONLY|RDWR)` 掩码冲突，只读打开被误判为写 | 改为 `mode = flags & 3`，只拒绝 WRONLY/RDWR |
| DHCP/DNS 永远超时（即使 ping 通） | `net_recvfrom(MSG_DONTWAIT)` 在 `net_dgram_recv()` 之前返回 `-EAGAIN`，轮询路径不驱动 RX 环 | 等待循环自行 `net_poll()`，超时改为有界预算 |
| USB HID 描述符全部解析失败 | EHCI qTD token 剩余字节位于 bits 30:16；HID report descriptor 必须用 `bmRequestType=0x81` | 修正位域与请求类型；HID 字符并入 shell 输入环 |
| SB16 无声/只录到 44 字节头 | `0xC0` 必须带 format 字节；方波累加器需 `+=2*freq`；DMA 缓冲须落在 24 位地址内 | 修正命令序列与累加器，新增 `.sb16_dma ALIGN(64K)` 段 |
| FAT32 `sync` 后数据未落盘 | `fatfs_sync()` 只刷缓存未刷设备 | 补 `blk_flush(g_vol->drive)` |
| `mmap` 失败可能返回物理页 0 | 失败路径未回滚已映射页 | 回滚并返回 `-ENOMEM`；`munmap` 回收非 memfd 帧 |

## ■ 新增能力

**引导 / 时钟**
- **自写 64 位 BIOS 引导链**（MBR stage1 → stage2 16/32 位 → long mode），与 Limine 共用同一 `kernel.elf`。
- 真实 **HPET** 驱动；LAPIC 定时器改用 HPET 校准（8254 mode3 半速修正）；通用时基 `time_source`（HPET 100 MHz 优先 + jiffies 回退）。

**文件系统**
- **ext4 只读**：extent tree depth 0..4、sparse hole、fast/slow symlink；不支持特性按位拒绝。
- **VimtuFS2 只读**：v5/v4/v3，d0..d3/ind/dind 块映射，格式特性位拒绝。
- **OS67 inode FS 只读**（Minix v1 布局）：inode 缓存/引用计数、直接+单级间接块映射。
- **MayFS 可写**（双槽校验和快照）：FNV-1a 校验、generation 选新、先备后主提交、掉电恢复。
- 合成 **procfs**（meminfo/uptime/version/cpuinfo/mounts）与 **sysfs**（kernel/ + block/）。

**USB / 音频 / 显示**
- **USB HID 键盘/鼠标**（boot 协议）+ root-port **热插拔重扫**，`usbinfo` 命令。
- **SB16 + 8237 DMA** 播放（8-bit mono 22.05 kHz）；**AC'97 输出音量**；**HDA 输出音量**。
- **virtio-gpu 2D**（modern virtio 1.0）与 **VMware SVGA II（vmsvga）2D**，均为显式 opt-in，默认启动不触碰。

**网络**
- **DHCP 客户端**（DISCOVER/OFFER/REQUEST/ACK）与 **DNS A 记录解析**，`dhcp`/`dns` 命令，`wget` 支持主机名。

**文档**
- `docs/porting-domains-report.md`（逐域移植报告）、`docs/porting-final-summary.md`（收口清单）、
  `docs/porting-source-map.md`（**64 个候选来源**的逐来源状态表）、双语 README（`README.md` / `README.zh-CN.md`）。

## ■ 验证

同一份最终内核实跑，全部通过：

```
selfboot 25/25 · hpet 13/13（自检 16/16）· time_source 10/10
procfs 9/9 · sysfs 9/9 · sb16 23/23 · ac97 音量 7/7 · hda 音量 9/9
usb_hid 14/14（存储回归 7/7）· virtio-gpu 27/27 · vmsvga 34/34
dhcpdns 17/17 · ext4 44/44 + 宿主 217/217
vimtufs2 62/62 + 宿主 173/173 · os67fs 14/14 + 宿主 128/128
mayfs 27/27 + 宿主 42/42 + recovery 13/13
GUI 外观门禁 0 像素 · symbol-scan 1381 符号 / 0 冲突
```

## ■ 产物

| 项 | 值 |
|---|---|
| 文件名 | `sutureos.iso` |
| 字节数 | 18,495,488 |
| SHA-256 | `73bc8cd84a65c0f782c5513642226c05d8acf9c956c4e07ab8b0b6c8e21bfaff` |
| 下载直链 | https://github.com/Fanqi89/SutureOS/releases/download/v0.57.0-alpha/sutureos.iso |
| 发布页 | https://github.com/Fanqi89/SutureOS/releases/tag/v0.57.0-alpha |

## ■ 来源与许可

- 上游来源版图：**64 个候选开源 OS 项目**；当前树实际使用 **32 个**（16 直接移植 + 16 参考重写），
  其余 8 个由已选实现等价覆盖、3 个待定/UNVERIFIED、21 个作为后续批次机制蓝本。逐来源状态见
  `docs/porting-source-map.md`；逐文件来源见 `docs/provenance.md`；许可登记见
  `THIRD_PARTY_LICENSES.md`（§1–§34）与 `docs/licensing-audit.md`。
- 本版新增登记：Uinxed-Kernel（Apache-2.0，ATAPI，§31）、Deshab-OS/Lnxrm（Apache-2.0，算法参考，§32）、
  flanterm（BSD-2，终端渲染器，§33）、musl（MIT，动态链接器参考，§34）；MayOS 实读许可为 **MIT**。

## ■ 已知限制

- **UEFI 启动未验证**（ISO 内含 UEFI El Torito 镜像，BIOS 为已验证路径）。
- 两套调度器（`task.c` 协作轮转 + EEVDF）仍并存；该路径上刻意避免任务上下文让出。
- 所有代码运行在 ring 0，`sudo` 是策略层而非安全边界。
- USB HID：仅 high-speed、仅 boot 协议设备；无 LED/NKRO；热插拔靠 shell 空闲循环轮询。
- virtio-gpu / vmsvga：仅 2D，上限 1024×768，无 3D/硬件光标/vsync。
- SB16：仅 8-bit 单声道 22050 Hz，轮询完成判定。
- AC'97 音量：QEMU WAV 后端不把 mixer 增益写入录音，证据为寄存器回读；未在真机验证。
- DHCP/DNS：仅 A 记录；无租约续期、IPv6、TCP 回退。
- ext4 / VimtuFS2 / OS67FS：只读，且对支持的磁盘特性有明确取舍。
- MayFS：单文件硬上限 4095 字节、每卷 48 条记录。
- MayFS/ext4/VimtuFS2/OS67FS/vmsvga/SB16/USB HID 仅在 QEMU 下验证，真机验证仍在进行。
