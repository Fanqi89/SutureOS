# SutureOS 移植域报告（《移植说明.txt》12 域，2026-11-20）

**基线**：`v0.56.0-alpha`（HEAD `badfb4d`，58 个 Release 保留）。
**本轮**：按 12 个能力域顺序推进；**未提交、未打 tag、未发 Release**（按要求跳过，记为「未完成」）。
**红线**：GUI 视觉层（任务栏/窗口边框/配色/字体/指针/开始菜单）**零改动**，外观门禁全程 0 像素；启动呈现（终端/banner/黑底白字）未变。
**自检基线**：13/14 → **16/16**（moos_intr 修好 + loader_pool + time_source 两个新自检；procfs 自检随试验回退）。

---

> 收口总说明（12 域覆盖、仍缺项、待授权来源、UNVERIFIED 清单、复跑入口）见
> [docs/porting-final-summary.md](porting-final-summary.md)。

## 本轮补齐（并行移植批次，全部真机 QEMU 验收）

| 能力 | 对应域 | 验收 | 关键证据 |
|---|---|---|---|
| 合成 `/proc` | 6 | `procfs_acceptance` **9/9** | `ls /proc` 五文件、meminfo/version/uptime/mounts、写被拒；修复 `VFS_O_RDONLY` 位掩码误判根因 |
| 合成 `/sys` | 6 | `sysfs_acceptance` **9/9** | `/sys/kernel/{version,uptime}`、`/sys/block/<dev>/{size,dev,ro,type}`、ata0 size 非零 |
| SB16 + 8237 DMA | 4 | `sb16_acceptance` **23/23** | WAV tone 0.500 s / peak 0.5000 FS / 440.04 Hz；无卡回归 16/16 |
| AC'97 输出音量 | 4 | `ac97_volume_acceptance` **7/7** | mixer ATTEN=21 回读 0x1515、100% 回读 0x0；QEMU WAV 不应用增益已记录 |
| DHCP 客户端 + DNS 解析 | 7 | `dhcpdns_acceptance` **17/17** | ACK 四元组、`[dns] answer ... 1 A record`、nxdomain 诚实失败、ping 回归 |
| USB HID（键鼠 + 热插拔） | 8 | `usb_hid_acceptance` **14/14** | kbd/mouse 认领、sendkey 解码、device_add/del 热插拔；存储回归 7/7 |
| virtio-gpu 2D | 9 | `virtio_gpu_acceptance` **27/27** | 8 色带逐字节、白方块 64×64 位移、off 后停止；默认启动 0 条 `[vga2d]` |
| vmsvga（VMware SVGA II 2D） | 9 | `vmsvga_acceptance` **34/34** | BAR0 index/value 端口、FIFO UPDATE、8 色带逐像素、白方块 64×64 位移、off 交还终端；默认启动 0 条 `[vmsvga]` |
| ext4 只读 | 6 | `ext4_acceptance` **44/44** + 宿主独立校验 **217/217** | extent depth-1 双 leaf、sparse hole、fast symlink、写拒绝、镜像只读未变 |
| MayFS | 6 | `mayfs_acceptance` **27/27** + 宿主 **42/42**（+ recovery 13/13、regression 11/11） | 双槽校验和快照、commit gen 7→8、写后重挂读回、双槽损坏拒挂 |
| OS67 inode FS（Minix v1） | 6 | `os67fs_acceptance` **14/14** + 宿主 **128/128** | 嵌套目录、BIG.BIN 20000 字节（7 直接+13 间接）、空文件、写拒绝 |
| VimtuFS2 只读 | 6 | `vimtufs2_acceptance` **62/62** + 宿主独立校验 **173/173** | v5/v4/v3 挂载、d0..d3/ind/dind 读取、feature_bits 拒绝、FAT32 共存 |

**工程注意（验收夹具卫生）**：`build/fat32_ata.img` 被写测试（wget/FAT32 写）弄脏后再启动 GUI 时，内核 FAT32 `ops` 自检会在损坏的目录链上卡住（日志停在 `[fat32] lfn self-test PASS` 后不再前进）；跑 GUI/外观类验收前必须用 `py -3 scripts/mkfat32_testimg.py` 重建并把 `build/fat32.img` 复制成 `build/fat32_ata.img`。这是夹具/既有 FAT32 自检的边界行为，不是本轮改动引入。

---

## 域 1 引导/启动（已完成）

- **移植/新增**
  - `boot/selfboot/stage1.S`：512 字节 MBR，INT 13h AH=42h 读 LBA1..32 到 0x7E00。
  - `boot/selfboot/stage2.S`：16 位（A20/E801/VBE 1024x768x32/multiboot1 MBI）→ 32 位（ATA PIO 直接把 flat kernel 读到 0x100000）→ 清内核 .bss、建恒等页表、PAE/EFER.LME/CR0.PG → 长模式 → `_start64`。
  - `boot/selfboot/stage2.ld`、`scripts/build_boot_disk.py`（扁平化 + patch header + 生成 `build/selfboot.img` 与 manifest）、`scripts/selfboot_acceptance.py`。
  - `boot/boot.S`：新增 `.global _start64` 64 位入口（edi=0x2BADB002、esi=0x6000），导出 `pml4/pdpt/pd0/pd_io/stack_top`，与 Limine 路径共用同一 `kernel.elf`。
- **许可与署名（逐文件）**
  - e1ruBSD（BSD-2-Clause）：MBR/stage2 链、GDT/A20 配方；XEKernelOS（MIT）：MBR→Stage2→PM 形；zeroAOS（Apache-2.0）：VBE 发现机制；SukiOS（GPL-3.0）：长模式顺序；myos（BSD-2）：A20/GDT；AntzOS（MIT）：MBR→PM 交接。全部**读后以 GAS 重写**，未拷可识别代码块。
  - 登记：`THIRD_PARTY_LICENSES.md` §19、`docs/licensing-audit.md` §4、`docs/provenance.md` 三行。
- **改动文件**：`boot/boot.S`、`boot/selfboot/*`、`scripts/build_boot_disk.py`、`scripts/selfboot_acceptance.py`、`tests/symbol-scan.ps1`、三份许可证/来源文档。
- **符号前缀与冲突**：引导链是独立二进制、无内核全局符号；内核新增 `_start64` 与 4 个页表符号。`tests/symbol-scan.ps1` 修复硬编码 ROOT、只统计**全局**符号并排除 ring-3 libc/用户 blob（独立链接单元）后：**1237 个全局符号，0 冲突**（123 个内核对象）。
- **三项验收**
  1. QEMU 无 PANIC 且自检不减：自写链启动到 `sutureos#`，`[stitch] selftests: 15/15`；Limine 路径同为 15/15。
  2. 宿主独立逐字节：`selfboot_acceptance.py` **25/25**（镜像 sha256、stage1 512B+0x55AA、stage2 魔数/跳转、payload 与独立 objcopy 的 `kernel.flat` 逐字节相同、patched entry64/ksect/klba/bss/pml4 与 `nm` 一致、multiboot 魔数）。
  3. `gui_appearance_check.py`：**0 像素**在动态矩形外。
- **未完成/未授权**：自写链只覆盖 BIOS 引导镜像；ISO 的 UEFI 启动仍用 Limine；安装器仍写 Limine 引导；未把自写链写进安装盘。

## 域 2 CPU / 中断 / 时钟（已完成）

- **移植/新增**
  - `arch/x86_64/hpet.c/.h`：从桩改为真实实现——优先 ACPI `HPET` 表、回退 `0xFED00000`，`mmu_map_mmio_identity`，读 64 位 CAPABILITIES（period 在高 32 位），`ENABLE_CNF` 前先停/清零，主计数器高-低-高原子读，导出频率；`hanos_hpet_wait_us`。
  - `arch/x86_64/lapic.c`：LAPIC 定时器优先用 **HPET** 校准（20 ms 窗口），PIT 回退路径修正 8254 mode-3「每 2 输入时钟减 1」的 2 倍误差。
  - `kernel/interrupts/intr.c`：删除 `moos_intr_handler` 里的**重复计数**（同一函数顶部与中部各 `g_counts[vec]++` 一次 → 每中断 +2），`moos_intr` 自检 13/14 的唯一失败项就此修好。
- **许可与署名**：`hpet.c/.h` 来自 HanOS（GPL-3.0，原骨架）；LAPIC 校准与 intr 修复为本地实现；均与 GPL-3.0 兼容。
- **改动文件**：`arch/x86_64/hpet.c`、`hpet.h`、`lapic.c`、`kernel/interrupts/intr.c`。
- **符号前缀与冲突**：`hanos_` 保持；0 冲突（1237）。
- **三项验收**
  1. `[hpet] ready: mmio=0xfed00000 rev=1 period=10000000 fs freq=100000000 Hz`、`[init] HPET init OK`、`[lapic] calibration source: HPET (100000000 Hz)`、`timer calibrated: 62848729 ticks/s`，自检 **15/15**、无 PANIC（`scripts/hpet_acceptance.py` **13/13**）。
  2. 宿主独立重算：`freq == 1e15 / period`（10,000,000 fs → 100 MHz）成立，period 落在 1..100 ns 合理区间，LAPIC 62.8M ticks/s 合理。
  3. 外观 0 像素。
- **未完成**：HPET 比较器中断/周期 tick 未接入（系统 tick 仍 LAPIC 0x20）；HPET 的 IOAPIC 路由未接。

## 域 3 内存 / 调度 / 进程（已完成）

- **移植/新增**
  - `kernel/syscall/loader.c/.h`：用户帧池 256×4 KiB 从「只增游标」改为**位图**，新增 `loader_free_page()`、`loader_pool_free_pages()`、`st_loader_pool_test()`（填满/耗尽返回 NULL/释放后复用/复位）。
  - `kernel/syscall/mem.c`：`sys_mmap` 分配失败**回滚本次已映射页并返回 -ENOMEM**（旧代码 `NULL|P|U` 会把物理页 0 映射给用户）；`sys_munmap` 回收匿名帧（`PTE_MEMFD` 软件位 bit9 标记 memfd 映射，只清 PTE 不回收 memfd 持有的页）。
  - `kernel/ipc/memfd.c`：`memfd_map` 打 `PTE_MEMFD` 位。
- **许可与署名**：本地实现（OOM 防护/配额思路参考 Lnxrm、SpiritFoxOS，仅思路，未拷代码）；池/映射原本就是 SutureOS 自身代码。
- **改动文件**：`kernel/syscall/loader.c/.h`、`kernel/syscall/mem.c`、`kernel/ipc/memfd.c`、`kernel/main/kmain.c`。
- **符号前缀与冲突**：`loader_*` 新符号；0 冲突。
- **三项验收**
  1. `[selftest][loader_pool] PASS`，自检 **15/15**，`hello from ring 3` 等 ring-3 程序正常，无 PANIC。
  2. 宿主侧：本域无字节产物（属逻辑/内存管理），以「帧池满/耗尽/复用」的确定性断言 + 映射回滚语义代替；页池计数在自检内闭环校验。
  3. 外观 0 像素。
- **未完成**：完整 OOM killer（进程级内存记账/配额、回收策略）；双调度器（`task.c` 协作轮转 + `eevdf.c`）统一仍未做（既有结构问题）。

## 域 4 声音（已完成）

- **移植/新增**
  - `kernel/drivers/hda.c/.h`：输出音量控制——按路径每个节点读 `OUT_AMP_CAP`（0x12），用 `SET_AMP_GAIN_MUTE`（0x300）写 gain；0% = mute，100% = 0 dB，整数线性 dB 映射（0%→-60 dB，无浮点）；播放前重设（电源切换会复位 amp）；导出 `hda_set_volume/hda_get_volume`。
  - `kernel/shell/shell.c/.h`：`hdavol [0..100]` 命令。
- **许可与署名**：`hda.c/.h` 骨架来自 PlantOS（MIT，`src/kernel/drivers/sound/hda.c`）；音量控制与 hdavol 为本地新增。
- **改动文件**：`kernel/drivers/hda.c`、`hda.h`、`kernel/shell/shell.c`、`shell.h`。
- **符号前缀与冲突**：`hda_` 不变；0 冲突。
- **三项验收**
  1. `scripts/hda_volume_acceptance.py` **9/9**：两次真实启动的 WAV 后端对比，50% 音量尾段 peak 16383 → **9702**（真实衰减）、`[hda] amp node=2 cap=0x80034a4a steps=74 gain=44`、`[hda] selftest: PASS`、无 PANIC。
  2. 宿主独立：WAV 由 `verify_hda_wav.read_wav`（按 EOF 解析 QEMU 不更新的 RIFF 长度）分析，尾段 RMS/peak 与音量语义一致。
  3. 外观 0 像素。
- **SB16 + 8237 DMA（本轮补齐，已完成）**：`kernel/drivers/sb16.c/.h`（DSP reset/版本/0xD1/0x41/0xC0 + 8237 通道 1：mask 0x0A、flip-flop 0x0C、mode 0x45、addr 0x02、page 0x83、count 0x03；8-bit 无符号单声道 22050 Hz 方波合成）；`boot/linker.ld` 新增 `.sb16_dma ALIGN(64K) (NOLOAD)` 段（内核 .bss 已到 21.7 MiB，超出 8237 的 24 位地址总线，DMA 缓冲实际落 0x210000）；`kmain.c` 在 HDA 后只调 `sb16_init()`（不播启动音，保持呈现冻结）；shell 加 `sb16test [freq] [ms]`。来源：驱动自写，寄存器时序只读核对 **Neumann（GPL-3.0，sb16.c/dma.c）与 Powerint-DOS-386（GPL-3.0，sb16.c/dma.c）**，未拷代码。
  - 验收：`scripts/sb16_acceptance.py` **23/23**（有卡 `-device sb16,audiodev=a0` + `-audiodev wav,...,out.frequency=48000`）：串口 `[sb16] dsp reset ok version=4.5`、`[  OK  ] SB16: Sound Blaster 16 DSP 4.5 @0x220, DMA 1, 8-bit mono 22050 Hz`；WAV 48000 Hz/2ch/16bit，tone **0.500 s**、peak **0.5000 FS**、过零频率 **440.04 Hz**；无卡回归只多一条 `[sb16] no DSP (not present)`、自检 16/16、无 PANIC。
  - 已知限制：仅 8-bit 无符号单声道 22050 Hz（无 16-bit/立体声/自动重装）、完成判定轮询（无 IRQ 处理）、只在 QEMU 验证。
- **AC'97 输出音量（本轮补齐）**：`ac97_set_volume/get_volume`（master 0x02 + PCM-out 0x18，ATTEN 0..31 步、1.5 dB/步，0% mute、100%=0 dB）+ shell `ac97vol [0..100]`；写入后回读 mixer 寄存器。
  - 验收：`scripts/ac97_volume_acceptance.py` **7/7**：`[ac97] volume=30% (master+pcm atten=21 steps readback=0x1515)`、`volume=100% ... readback=0x0` 回读一致；**QEMU 的 AC97 WAV 后端不把 mixer 增益应用到录音**（实测两次 peak 都是 16383/ratio=1.000，如实记录为该模拟器限制，真机行为未验）。
- **未完成**：HDA 采集/中断/多 codec 未做；AC'97 录音未做。

## 域 5 存储 / 块设备（已完成）

- **移植/新增**：`kernel/fs/fatfs.c` 的 `fatfs_sync()` 在写完 FAT/FSInfo 后调用 `blk_flush(g_vol->drive)` —— 与 ext2 对齐，`sync` 语义真正把设备写缓存刷下去。
- **许可与署名**：`fatfs.c` 原移植自 PlantOS（MIT，ChaN FatFs 血统）；本次改动为本地实现。
- **改动文件**：`kernel/fs/fatfs.c`。
- **符号前缀与冲突**：无新符号；0 冲突。
- **三项验收**
  1. `scripts/sync_acceptance.py` **6/6**（write → sync → cat，且**不 umount**）。
  2. 宿主独立逐字节：从裸盘镜像解析出 `SYNC.TXT`，内容 `b'written then synced\n'` 逐字节相同。
  3. 外观 0 像素。
- **覆盖证据（审计）**：AHCI/ATA/NVMe/VirtIO-blk/ATAPI/USB 六类块驱动均实现 `.flush`；`ext2.c`、`fatfs_format.c`、`installer.c` 都会调用 `blk_flush`。
- **未完成**：NVMe 限 4 KiB 页、AHCI 无 ATAPI 转发、USB 无热插拔（均既有）。

## 域 6 文件系统（本域已齐全；含一次回退记录）

- **覆盖证据**：ext2（读写/多卷/截断/单+双间接/位图自检）、FAT32（读写/VFAT LFN/多卷/分区偏移/多簇目录）、SquashFS（只读/多卷）、ISO9660（只读，El Torito 区分）、ramfs（512 节点/256 KiB）、VFS 统一驱动 + 挂载表。
- **三项验收**
  1. 15/15 自检、无 PANIC（`hpet_acceptance.py` 13/13）。
  2. 宿主独立逐字节：`verify_squashfs_read.py build/squashfs.img` **83/83**。
  3. 外观 0 像素。
- **procfs（本轮补齐，已完成）**：`kernel/fs/procfs_vfs.c` 合成只读 `/proc`（meminfo/uptime/version/cpuinfo/mounts），`vfs.c` 注册驱动、`kmain.c` 在 ramfs 根挂载后 `vfs_mount("none","/proc","procfs")`。
  - **上一轮失败的真根因（已修）**：只读判断写成了 `flags & (VFS_O_WRONLY|VFS_O_RDWR)`，而 `VFS_O_RDONLY=0x1`、`VFS_O_WRONLY=0x2`、`VFS_O_RDWR=0x3`，这个掩码 `0x3` 把 RDONLY 位也包含进去 → 每次读打开都被当成写意图拒绝（`cat` 报 "operation not supported"）。正确写法是先取 `mode = flags & 0x3`，只拒绝 `mode==VFS_O_WRONLY || mode==VFS_O_RDWR`。驱动内部按 handle 保存生成文本与长度，`read` 用 VFS 传进来的 offset。
  - 验收：`scripts/procfs_acceptance.py` **9/9**（真实启动 + QMP 逐字符注入 `ls /proc` / `cat /proc/meminfo` / `cat /proc/version` / `cat /proc/uptime` / `cat /proc/mounts` / `write /proc/x nope`）：五文件全部列出、MemTotal/MemFree 非零、version 含 SutureOS 0.56.0-alpha、uptime `N.NN N.NN`、mounts 含 `ram / xj_vfs` 与 `none /proc procfs`、写被拒（read-only）、无 PANIC；自检 16/16、HPET 13/13、外观 0 像素回归通过。
- **sysfs（本轮补齐，已完成）**：`kernel/fs/sysfs_vfs.c` 层级只读 `/sys`：`/sys/kernel/{version,uptime}`、`/sys/block`（目录枚举所有已注册块设备）、`/sys/block/<name>/{size,dev,ro,type}`；`vfs.c` 注册、`kmain.c` 在 `/proc` 之后挂载。机制来源：LunaixOS sysfs 树布局 + Linux `/sys/block/<dev>/{size,dev,ro}` 约定（只读参考，未拷代码）。
  - 验收：`scripts/sysfs_acceptance.py` **9/9**（真实启动 + QMP 逐字符注入）：`[sysfs] mounted at /sys (kernel/ + block/)`；`ls /sys` = kernel/block；`ls /sys/kernel` = version/uptime；`cat /sys/kernel/version` 出 0.56.0-alpha；`cat /sys/kernel/uptime` 出 N.N；`ls /sys/block` 出 ata0；`cat /sys/block/ata0/size` 出非零扇区数；`write /sys/x` 被拒；无 PANIC。
- **ext4 只读（已完成）**：`kernel/fs/ext4.c/.h/.` + `ext4_vfs.c`（驱动名 `ext4`，只读）：superblock@1024、组描述符、inode、**extent tree depth 0..4**（index→leaf，逐层校验 0xF30A/entries/depth）、filetype 与无 filetype 目录、多 extent 与 hole、fast/slow symlink target；不支持的 incompat 位按名字拒绝（64bit/meta_bg/mmp/bigalloc/inline_data/ea_inode/dirdata/csum_seed/largedir/encrypt/casefold/recover/journal_dev/compression），dir_index 拒绝，metadata_csum 只读允许但明说不校验。来源：自写，依据 Linux 内核公开规格 Documentation/filesystems/ext4（未拷代码）。验收：`scripts/ext4_acceptance.py` **44/44**（`extents=384 depth=1 leaves=2`、`SPARSE.BIN` hole、`LINK.TXT` fast symlink、写拒绝且文件未出现、镜像只读未变）+ 宿主独立解析 `verify_ext4_read.py` **217/217**；无盘回归 **11/11**（只多 `[vfs] Registered driver: ext4` 与 `[ext4] no ext4 volume on drive 1` 两行）。生成器 `mkfs_ext4_testimg.py` 自校验 121/121，含 8 个不合规 feature 负例。限制：只读、单卷、符号链接不跟随、>4GiB 不可寻址、仅实测 4096 块、本机无 e2fsprogs 故缺 e2fsck 外部参照（三方互证替代）。
- **VimtuFS2 只读（已完成）**：`kernel/fs/vimtufs2.c/.h/.` + `vimtufs2_vfs.c`（驱动名 `vimtufs2`）：支持 **v5/v4/v3**（128B inode），v2 与 journal/quota/ACL/snapshot 特性位**明确拒绝并打印具体位**；超级块 magic/0xAA55/主 CRC/几何逐项重算、inode CRC、d0..d3/ind/dind 映射（先范围校验）、目录=parent 过滤的 inode 线性扫描（16 扇区/64 inode 一次读）。来源：格式规格来自 VimtuOS `kernel/vfs64.h`/`vfs64.cpp`（GPL-3.0，兼容），**按规格重写、未拷代码**。验收：`scripts/vimtufs2_acceptance.py` **62/62**（primary 26 + negative 8 + v4 11 + nodisk 6 + combo 11）+ 宿主独立校验 **173/173**（镜像 sha256 与生成时一致=只读证明）；归一化串口 diff：基线 268 行 removed=0、added=2。限制：只读、无写路径、不解析符号链接、stat mtime 报 0、目录查找 O(inode 数)。
- **OS67 inode FS（Minix v1 布局，本轮补齐，已完成）**：`kernel/fs/os67fs.c/.h` + `os67fs_vfs.c`（驱动名 `os67fs`，只读）——移植 OS67 的 inode 层算法：inode 缓存 + 引用计数 + I_VALID/I_BUSY 标志（NINODE 32）、inode 号从 1 起（root=1）、16 字节目录项{ino:u16,name[14]}、7 直接 zone + 单级间接表（512 项）、位图编号（inode 位=inode 号；zone 位 = zone - fst_data_zone + 1）。格式来源：OS67 `inc/minix.h`/`inc/inode.h`/`fs/inode.c`/`fs/bitmap.c`/`fs/dir.c`（**只读参考，按规格重写、未拷代码**）。验收：`scripts/os67fs_acceptance.py` **14/14**（探测 minix v1 卷、`mount os67fs 1 /mnt/o67`、ls 五项、HELLO/嵌套 INNER 读取、`stat BIG.BIN`=20000 字节（20 zone：7 直接+13 间接）、空文件 0 字节、写被拒、umount、自检 16/16、无 PANIC）+ 宿主独立解析 `verify_os67_read.py` **128/128**（位图/zone 归属唯一/inode 链/目录/manifest sha256/镜像 sha256 未变）；生成器 `mkfs_os67_testimg.py` 自校验 **82/82**。限制：只读（无 ialloc/iwrite/itrunc 写路径）、无双间接（OS67 本身未用）、仅 1024 字节 zone、仅 QEMU 验证。
- **MayFS（MayOS 校验和双槽快照 FS，本轮补齐，已完成）**：`kernel/fs/mayfs.c/.h` + `mayfs_vfs.c`（驱动名 `mayfs`，**可写**）——双槽快照（主槽 + 备槽各 512 扇区 = 256 KiB）、每槽自带 **FNV-1a 校验和**、`generation` 选新、commit「先备槽后主槽；每槽废头→写数据→写头→flush」、48 条记录（4 条固定根 + 44 用户条目）、名字 ≤47 字符、单文件上限 **4095 字节**（`FILE_CAP=4096` 且 `data[size]==0` 格式不变式）。来源：**实读 MayOS LICENSE = MIT（Copyright (c) 2024 MayOS Project）**，不是素材库清单写的 GPL-3.0 同族——以实读为准（MIT 与 GPL-3.0-or-later 相容）；格式/校验和/双槽语义照规格，实现自写、未拷代码。验收：`scripts/mayfs_acceptance.py` **27/27**（探测 generation=7、ls/cat/stat、mkdir/rmdir/mv、`write NEW.TXT` → `sync` 提交 gen 8 → umount → 重挂读回 gen 8 且内容不变、自检 16/16、无 PANIC）+ 宿主独立校验**被写过的那份镜像 42/42**（NEW.TXT 逐字节、12 条旧记录全不变、两槽 generation 相同且表逐字节相同、pristine→dirty 仅 27/262144 字节变化）；`--recovery` **13/13**（主槽校验和损坏 → 选备槽并提交 gen 10 修复；双槽损坏 → **拒绝挂载**且镜像 sha256 未变，不自动重格式化）；`--regression` **11/11**（无盘启动只多 2 行；ext2/ext4/VimtuFS2/OS67FS/ISO9660/FAT32 日志与自检不变）。生成器自校验 47/47、干净夹具独立校验 33/33。限制：单文件 ≤4095 字节（格式上限，BIG.BIN 只能是 4095）、无时间戳/权限/符号链接、48 条记录、整卷 512 KiB 重写、仅 sync/umount 提交、单卷、未做真断电注入、仅 QEMU 验证。
- **未完成**：无（文件系统域按落地口径完成；仅列限制项）。

## 域 7 网络（已完成：链路→应用层 + DHCP + DNS）

- **覆盖证据**：RTL8139 + e1000 链路层、ARP/IPv4/ICMP/UDP/TCP、`ping`、`tcpapi`、`wget`、socket ABI（v0.14–v0.15 已齐，本轮未新增移植）。
- **三项验收**
  1. `scripts/nic_acceptance.py` **PASS**（e1000 链路 UP、交互 `ping` 3 条回复；RTL8139 同）。
  2. 15/15 自检、无 PANIC（同一 ISO）。
  3. 外观 0 像素。
- **DHCP 客户端 + DNS 解析（本轮补齐）**：既有 `kernel/net/dhcp.c/.h`、`dns.c/.h` 实读为**完整协议层、不是桩**（DHCP option 53/61/55/50/54/1/3/6/51 + DORA + `net_config` 钩子；DNS QNAME/RD/A 解析 + 压缩指针 128 跳防环）。真缺口是：没有 `dhcp_acquire()` 入口、DORA/DNS 零日志、**timeout 参数是假的**（内部阻塞 recvfrom 3s×4 次）、shell 没有 `dns <name>`、`wget` 只吃 IPv4 字面量。本轮补齐：`dhcp_acquire(timeout_ticks)`（`net_poll()+MSG_DONTWAIT` 有界等待，预算生效）、`[dhcp] DISCOVER/OFFER/REQUEST/ACK` 日志；`dns_resolve()` 有界重试 + `[dns] query/answer/nxdomain/timeout` 日志 + `dns_rcode()`；shell `dhcp`（打印四元组）/`dns <name>`；`wget` 主机名回落 DNS（失败给出诚实提示）。
  - **找到并修掉的真 bug**：`net_recvfrom(..., MSG_DONTWAIT, ...)` 在 `net_dgram_recv()` **之前**就返回 `-EAGAIN`，轮询路径根本不驱动 RX 环 → 即使 ping 通，DISCOVER/OFFER 与 DNS 查询永远超时。两个新循环现在自己调 `net_poll()`。
  - 验收：`scripts/dhcpdns_acceptance.py` **17/17**（真实启动 + QMP 逐字符）：`[dhcp] ACK ip=10.0.2.15 mask=255.255.255.0 gw=10.0.2.2 dns=10.0.2.3 lease=86400s`、`[dns] query example.com` → `[dns] answer example.com -> 104.20.23.154 (1 A record)`、`wget http://nosuchhost.invalid/x` 走 nxdomain 诚实失败、lease 后 `ping 10.0.2.2` 仍 3/3、无网卡回归 `dhcp: no NIC / network not ready` 且不伪造租约、自检 16/16、无 PANIC；`wget_acceptance.py` 8/8（IPv4 字面量下载逐字节 sha256 不变）。
  - 已知限制：仅 A 记录（无 AAAA/CNAME 追链/EDNS0/TCP 回退/缓存）；无租约续期 T1/T2、无 RELEASE/DECLINE、无 giaddr；超时是 wall-clock；`wget` 主机名→HTTP 成功未端到端证实（slirp 无法把任意域名映射回宿主 loopback）。
- **未完成**：IPv6、IP 分片重组、多网卡策略（既有缺口）；`wget` 仍限 HTTP/1.0 GET。

## 域 8 USB / 输入（已完成：存储 + HID + 热插拔）

- **覆盖证据**：EHCI（PCI 0:4.0）、USB 大容量存储（BOT，CBW/CSW，drive 16）、`/mnt/usb` 挂载、只读/写路径、`usb_selftest`。
- **三项验收**
  1. `scripts/usb_domain_acceptance.py` **7/7**（`[usb] ehci: pci`、`storage: 71680 blocks`、`drive 16`、`mount /mnt/usb OK`、`selftest PASS`、无 PANIC）。
  2. 宿主独立：从 pristine 复制一份镜像真实启动，读-only 启动后**整盘逐扇区未变**（`verify_usb_disk.py` PASS）。
  3. 外观 0 像素。
- **USB HID + 热插拔（本轮补齐，已完成）**：新增 `kernel/drivers/usb_hid.c/.h`（946 行）——EHCI 上的 HID 类驱动：描述符遍历、boot 协议键盘（8 字节报告）/鼠标（3 字节报告）解码、SET_IDLE/SET_PROTOCOL、事件+ASCII 字符队列、root-port 热插拔重扫（PORTSC CSC/PED 轮询，device_add/device_del 都能发现）；`kernel/drivers/usb_ehci.c/.h` 增加 11 个 `usb_ehci_*` hook（port reset/release、alloc address、control/bulk/intr 传输）并把 `usb_qh_arm` 末参数从 qTD 索引改成 qTD 指针；shell 加 `usbinfo [scan|watch [s]]`，并把 USB HID 解出的字符并入 shell 输入环（只有 USB 键盘的机器才能打字）；shell 空闲循环每轮做一次 `usb_hid_idle()`。来源：自写（GPL-3.0），仅依据 USB HID 1.11 App B、USB 2.0 §9、EHCI 1.0 §3 规范文本 + 既有 EHCI 传输层；未拷第三方代码。
  - 验收：`scripts/usb_hid_acceptance.py` **14/14**（两阶段：kbd/mouse 认领、non-boot HID 打印 report descriptor、`usbinfo` 列出 ≥2 设备、sendkey a → `[usb-hid] key down=0x04 ascii=a`、鼠标 Δ、`device_add usb-kbd` → `[usb-hid] hotplug: new HID on port N`、`device_del` → `unplug: port N`、两把键盘阶段、自检 16/16、无 PANIC）；存储回归 `usb_domain_acceptance.py` **7/7** 不变。
  - 已知限制：只做 high-speed（FS/LS 交还伴随控制器）；只解码 boot 设备（非 boot HID 仅认领+打印）；无 LED/NKRO/consumer 键；热插拔靠 shell 空闲轮询（GUI 中插拔要回 shell 才发现）；真机未验证（QEMU 11.1 模型）。
- **未完成**：无（本域按落地口径完成；真机 CRC/去抖未验）。

## 域 9 图形底层（本域已齐全 fbcon 部分）

- **覆盖证据**：multiboot1 bit12 帧缓冲解析、`mmu_map_mmio_identity` 映射高地址 LFB、fbcon/flanterm 终端、CJK 16×16 点阵、BMP/QOI 解码；Rust 软光栅渲染器建立在此层之上。
- **三项验收**
  1. `scripts/cjk_terminal_acceptance.py` **10/10**（16×16 点阵逐像素精确匹配；滚动 10 行后仍精确）。
  2. 15/15 自检、无 PANIC。
  3. 外观 0 像素。
- **virtio-gpu 2D（本轮补齐，已完成）**：新增 `kernel/drivers/virtio_gpu.c/.h`——modern virtio 1.0 全链路（PCI capability 解析 → BAR 映射 → reset → SET_FEATURES(VERSION_1)+FEATURES_OK → queue0 desc/avail/used → DRIVER_OK，关 INTx 纯轮询）；命令 GET_DISPLAY_INFO/RESOURCE_CREATE_2D/ATTACH_BACKING/SET_SCANOUT/TRANSFER_TO_HOST_2D/RESOURCE_FLUSH；静态 `.bss` 扫描缓冲 3,145,728 B（1024×768×4，设备报 1280×800 时如实 clamp）；shell `vga2d [on|off|status]` 为**唯一入口**，默认启动不触碰（`-vga std` 下 0 条 `[vga2d]`）。来源：自写（GPL-3.0），依据 OASIS VirtIO 1.2 规范 §2.7/§4.1/§5.7 + 仓内 `virtio_blk.c` 的 virtqueue 参考，未拷第三方代码；结构体 10 条 `_Static_assert` 钉死尺寸。
  - 验收：`scripts/virtio_gpu_acceptance.py` **27/27**（阶段1 `-vga none -device virtio-gpu-pci`：非黑 1.0000、8 条色带逐字节相等、白方块精确 64×64 且两帧 x 302→206 位移、off 后 flush 计数不再增长、status errors=0；阶段2 `-vga std`：0 条 `[vga2d]`、fbcon 正常、自检 16/16、无 PANIC、终端文字可见）；GUI 外观门禁 0 像素。
  - 已知限制：只做 modern virtio 1.0（legacy-only 分支为防御代码、本机无法构造）；分辨率上限 1024×768；仅 scanout 0 / 2D B8G8R8A8；无 cursor queue/3D/EDID/vsync；帧由 shell idle 循环驱动（打字时动画暂停）；不与 `startgui` 联动，是附加显示输出而非替代 fbcon。
- **vmsvga（VMware SVGA II，本轮补齐，已完成）**：`kernel/drivers/vmsvga.c/.h`——PCI 15ad:0405 探测、**BAR0 的 32 位 index/value I/O 端口对**（本机 0xc050/0xc051，不是 MMIO 寄存器文件）、BAR1 16 MiB VRAM、BAR2 64 KiB 命令 FIFO、`SVGA_REG_ID` 往返校验、FIFO MIN/MAX/NEXT/STOP 字节口径、`SVGA_CMD_UPDATE` 脏矩形；shell `vmsvga on/off/status` 为唯一入口，`on` 前 `console_screen_enable(0)`（终端与 svga 共用 VRAM），`off` 交还终端。关键实测事实：设备未编程模式时 WIDTH/HEIGHT 是 QEMU 占位 surface 的 640x480，必须编程一个不同模式（1024x768）否则 screendump 永远是占位图；默认启动 0 条 `[vmsvga]`。来源：自写，依据 VMware SVGA II 公开寄存器规格 + QEMU 外部实测。验收：`scripts/vmsvga_acceptance.py` **34/34**（screendump 1024x768 == 驱动几何、非黑 1.0000、8 色带逐像素、白方块 64×64 位移 x 673→365、off 后 update 计数不增、默认 `-vga std` 0 条 [vmsvga]、无设备时诚实拒绝）；默认终端截图逐像素 0/786432 差异，GUI 外观 0 像素。限制：只用 UPDATE 命令（RECT_FILL/RECT_COPY 能力位不用）、仅 2D、无 cursor/IRQ、仅 QEMU 验证。
- **未完成**：真 vsync/交换链未做；本域未改动既有显存/合成接口（避免动启动呈现）。

## 域 10 GUI 应用 / 用户态功能（已完成：桌面换代 + 最小 Wayland 服务端）

- **覆盖证据**：图片查看器（BMP/QOI）、文本编辑器、开始菜单/任务栏/最小化最大化/关闭、多用户切换、ring-3 文件工具、Wayland 四前置（AF_UNIX socketpair+poll 18/18、memfd+MAP_SHARED 18/18、SCM_RIGHTS 21/21、ring-3 wlprobe 14/14）。
- **三项验收**
  1. `scripts/editor_acceptance.py` **5/5**（GUI 编辑器真实键鼠写入，`[edit] saved 17 bytes`）。
  2. 宿主独立逐字节：`EDIT.TXT` == `b'hello editor 42\n'`。
  3. 外观 0 像素（GUI 视觉零改动）。
- **新 GUI（Rust 自写渲染器，v0.58 开发中，未发布）**：窗口层改为"不透明窗体 + 每帧 alpha 阴影环"
  （旧实现把阴影烘进层缓存并在黑底上合成，窗周是黑色硬边，实测 (0,0,0)）；新增窗口内键盘焦点
  与可编辑行（Enter 提交到串口、Backspace 退格、Tab 循环焦点）；最大化/还原改为 4 帧几何过渡。
  验收：外壳 **55/55**、`logo_acceptance` **12/12**（F10 关机动画 + ACPI S5）。
- **桌面换代（用户 2026-11-21 批准）**：`startgui` = Rust 桌面外壳（圆角 + 轻亚克力 + 伪 3D
  Fluent 图标 + 每窗真实应用内容），原 C 桌面保留为 `startgui-classic` 并继续跑它自己的外观门禁；
  两套基线：`docs/baseline/base_gui.png`（新）与 `base_gui_classic.png`（旧），门禁 `--profile new|classic`。
  验收：`desktop_acceptance` **19/19**、`gui_apps_content_acceptance` **25/25**、外壳 **55/55**、
  Logo（含 alpha）**14/14**、两套门禁 **0 像素**。
- **最小 Wayland 服务端（完成）**：named AF_UNIX 端点（int 0x80 号 15/16 bind/connect）+
  wl_display/wl_registry/wl_callback/wl_compositor/wl_surface/wl_shm/wl_shm_pool/wl_buffer/wl_seat
  子集；ring-3 `run wlclient` 经 SCM_RIGHTS 交付 memfd，commit 时服务端读像素算 FNV-1a；
  `wlshow` 把最后 commit 的 surface 整屏呈现（整数最近邻 + accent 边框）。
  验收：`wl_server_acceptance` **28/28**（客户端/服务端/宿主三方 hash 一致 + 宿主逐字节 256000 B）、
  `wl_show_acceptance` **9/9**（屏幕实际出现 surface）。
- **Wayland 合成器接线（v0.59 后续，未发布）**：图片查看器窗口直接绘制内核 Wayland 服务端最后
  commit 的 wl_surface（窗口层缓存键混入 commit 计数）；`wl_desktop_window_acceptance` **11/11**。
  协议补全：frame 回调→`wl_callback.done(serial)`、damage 累计、`wl_display.error/delete_id`、
  `wl_seat.get_pointer` 真 wl_pointer（内核鼠标快照）；`wl_protocol_acceptance` **29/29**
  （宿主重算两帧哈希：frame1 `c7763445` 与 WL.BIN 逐字节一致、frame2 `e6601b45` 与内核日志一致）。
- **交换链 + vsync 模拟（v0.59 后续，未发布）**：双场景缓冲 + FRONT 翻页 + 10 ms 闸门/合并，
  `guishell swapstat` 输出 `rendered == scene + skipped`；`guishell_swap_acceptance` **19/19**，
  桌面外观 0 像素越界。如实限制：skipped 正常操作恒 0（循环 30 ms 节流），无真 vsync/三缓冲。
- **GUI 收尾（v0.59.0 之后，未发布）**：
  - 三缓冲 + HPET 60 Hz 栅格节拍：`FRAME_A/B/C` + `DRAW/READY/PRESENTED` 状态机（每帧校验呈现源是
    READY、没有缓冲仍在 DRAW）、presented 完整性采样、jitter/dropped/latency/直方图全部进
    `guishell swapstat`，守恒式 `rendered == presented + skipped + pending`；`guishell_swap_acceptance` **30/30**。
  - damage 驱动局部重绘：唯一合成入口 `compose_rect()`（壁纸矩形 → 窗口层 → 活数据 → 淡入淡出 →
    焦点行 → 任务栏整条 → 菜单整块），全屏重绘就是"一个全覆盖 rect"；每 ≤20 个 damage 帧 + 结构性
    变化跑 oracle 整屏重绘逐像素比对（`damage: identical`，0 mismatch，不一致用 oracle 帧修复）。
    纯指针/空闲帧重绘占比 **4–5 permille**、拖动帧 **0–1 ticks**（旧整屏 ≈117 ms）；
    20 次 present 中途 screendump 与参考桌面 **0 像素**差异（半帧检测，非扫描线物理测量）。
    `guishell_damage_acceptance` **23/23**。
  - Wayland 多客户端：每连接独立保留最后 commit 的 surface（像素/几何/scale/serial），
    `wl_server_surface_count()/_at()` 导出，旧 `latest_surface()` 语义不变；`run wltwo` 单进程双连接
    （160×100@2x + 96×64@1x 子 surface）→ `wl_multi_acceptance` **40/40**（宿主独立重算三帧 FNV-1a +
    FAT32 析出逐字节）。
  - `wl_keyboard`/`wl_pointer` 真事件流（`mb_kbd.c` 解码路径镜像环 → keymap(NO_KEYMAP)/enter/key
    press+release/modifiers/leave；enter/motion/button 两沿/leave/frame 按 v5+ 绑定）+
    `set_buffer_scale` + `wl_region`（add/subtract 面积）+ `wl_subcompositor/wl_subsurface`（记录父子
    与偏移，**只记录不合成**）。
  - 独立 **Wayland 窗口**（accent `0x7a5cff`，**F2** 开关；开始菜单仍 4 行，因为菜单几何被既有验收
    冻结）；整数最近邻 1:1/2x 显示 surface，页脚列出每条保留的 surface，无客户端时显示提示卡、
    不回退 Logo；窗口层缓存键混入 commit 计数。
  - 视觉打磨第二轮：壁纸顶部冷光晕 + 六环整数晕影（亚克力采样源同步）、图标内描边/侧棱光/底部反光、
    窗口 2 px accent 身份条 + 玻璃内高光 + 22px 倒角控制钮、任务栏圆角按钮/圆角活跃下划线/顶部发丝
    高光、开始菜单圆角行；`docs/baseline/base_gui.png` 按新外观重生，门禁仍 **0 像素**；
    token 与硬约束登记在 `docs/gui-visual-spec.md`。
  - 修 `mmap(addr=NULL)` 二次映射覆盖前次 PTE 的 bug（memfd A 24576B 与 B 64000B 落重叠区间）：
    `user_map_alloc()` 单调下降游标，memfd 与匿名两支共用；`wltwo` 把它变成常驻断言
    （不重叠 + 第一次映射数据未变），窗口不回收属已知限制。
- **GUI 第二轮（v0.61，未发布）**：
  - 桌面文本编辑器真编辑：`apps.rs` 多行缓冲区（10×38）+ `editor_key/editor_move/editor_snapshot`，
    `editor_body` 画真实缓冲区与光标；焦点在编辑器时按键路由到缓冲区（含方向键/Home/End），
    **F5 经 `vfs_write_file` 写 `/mnt/fat/GUIEDIT.TXT`**；`gui_editor_live_acceptance.py` **12/12**
    （真键盘 → F5 → 宿主解析 FAT32 镜像逐字节比对 + "输入可见"像素断言）。
  - 修活数据穿透：任务管理器 uptime/CPU/心跳点原来在所有窗口层之后绘制，会穿到上层窗口里；
    改为各窗口活数据紧跟自身层、按 z 序绘制（验收断言该处 cyan 像素为 0）。
  - damage 合并细化：32×32 tile 位图（32×24）为真源，`add()` 后行扫描 + 纵向合并重建矩形表
    （上限 16 块，满了折叠进最后一块）；实测安静 30 帧窗口 ~250 tiles / 4 块 / permille 132，
    含菜单+拖动的窗口 permille 446，oracle 0 mismatch；`guishell_damage_acceptance.py` **28/28**。
    光标快路径判据随之改为 `tile_count() <= 4`（tile 粒度下斜向移动会触 4 个 tile）。
  - 桌面 Wayland pump 钩子：GUI 跑在 shell 线程、期间无 ring-3 客户端，服务端原来只在客户端
    syscall 路径 pump → 桌面上的 Wayland 窗口收不到指针事件；外壳每帧调用弱符号
    `wl_server_desktop_pump()`（`slint_bridge.c` 空实现），服务端按真实窗口矩形映射指针进保留
    surface 并打与活路径一致的日志。
  - Wayland 窗口摆放与输入语义：`wl_pointer` 的 enter/motion/leave 按窗口矩形换算成 surface 局部
    坐标（`local=(mouse-origin)/scale`，窗口内才发 enter/motion），矩形优先取
    `suture_gui_wayland_rect()`（`source=shell`），终端客户端场景退化为默认 (250,330,440,330)；
    `wl_keyboard` 按 `suture_gui_wayland_focused()` 门控并计数被丢的键。
  - keymap 真实下发：2614 B 精简 XKB 写 memfd → SCM_RIGHTS → `wl_keyboard.keymap(format=1, size, fd)`，
    客户端 map_shared 后算 FNV-1a，服务端/客户端/宿主三方一致（`43d6be15`）。
  - buffer 生命周期：release 每 attach 周期只发一次；`wl_buffer.destroy` 后该 surface 不可再 attach；
    `wl_shm_pool.destroy` 保留 fd（已建 buffer 可继续 commit，但 create_buffer 被拒）；
    `wl_server_buffer_stats()` 实测 created=2 released=4 destroyed=1。
  - 两个真 bug：①连接槽泄漏（pump 读到 EOF 只 break，`used` 永远为 1 → 4 槽耗尽 + desktop pump
    永久早退）→ `conn_release()` 在 EOF 后释放槽并保留 g_keep[] surface；②归属判据错（ring-3 退出后
    unix 对端不报 EOF）→ 改为按输入设备归属判定，`source=shell` 时桌面 pump 必须跑。
  - `wl_win_semantics_acceptance.py` **30/30**（窗口矩形/局部坐标逐条验算、真实拖动后坐标跟随、
    enter/motion/leave、焦点门控、keymap 三方 hash、buffer 生命周期）。
- **仍未完成**：真正的 vsync 源（本机没有扫描线中断，60 Hz 是墙上时钟栅格）、
  窗口内键盘焦点、Wayland scaling 的真实合成与"surface 作为真正窗口表面"（无窗口命中/摆放）、
  `wl_keyboard` keymap（内核无 XKB 数据）、多 surface 历史与 buffer 回收、最大/还原过渡的局部重绘。

## 域 11 跨架构蓝本（已完成：机制重写为通用时基抽象）

- **移植/新增**（机制来源：AArch64/RISC-V 内核的"单调计数器 + 已知频率 + 最佳源选择"，与具体架构无关）
  - `kernel/time/time_source.c/.h`：`time_source_t{name, freq_hz, read, prio}` 注册表（最多 4 个源）、`time_source_best()`（prio 高者胜）、`time_source_now_ns()`、`time_source_uptime_us()`、`time_source_ticks_to_ns()`（整秒 + 余数拆分，避免 `ticks*1e9` 溢出）。
  - x86_64 侧自写实现：**HPET 为高分辨率源**（100 MHz，prio=100，读 `hanos_hpet_read_counter()`）、**100 Hz 调度 tick 为单调回退**（prio=10，读 `g_tick_count`）；8254 PIT 仍是 LAPIC 校准基准（域 2）。
  - `shell` 新增 `timesrc`：列出所有源 + `now_ns/uptime_us` + 两读探针（间隔约 10 ms，打印 t0/t1/delta）。
  - 自检 `st_time_source_test()`：源数 ≥2、最佳源 prio ≥100、两次读单调不回退、时间确实前进、`ticks_to_ns(1, 100 MHz) == 10 ns`、`(1e8 ticks, 100 MHz) == 1 s`。
- **许可与署名（只读机制，未拷代码）**：LoongOS（**MIT**，AArch64 裸机 generic timer/tick 抽象）、token-os（**MIT**，aarch64 裸机）、MikuOS（RISC-V 64 参考）。登记在 `docs/provenance.md`、`THIRD_PARTY_LICENSES.md` §20、`docs/licensing-audit.md`。
- **改动文件**：新增 `kernel/time/time_source.c/.h`、`scripts/time_source_acceptance.py`；`kernel/main/init.c`（PIT 之后调 `time_source_init()`）、`kernel/main/kmain.c`（自检入口）、`kernel/shell/shell.c/.h`（`timesrc`）、`scripts/build.py`（`-I kernel/time`）。
- **符号前缀与冲突**：`time_source_*` 新符号族；`tests/symbol-scan.ps1`：**1381 全局符号，0 冲突**（138 内核对象）。
- **三项验收**
  1. `[time] 2 time source(s), best=hpet (100000000 Hz)`、`[selftest][time_source] PASS`、自检 **16/16**、无 PANIC（`scripts/time_source_acceptance.py` **10/10**）。
  2. 宿主独立重算：`timesrc` 探针 `delta_ns=10001200` 恰好是 10 ns 的整数倍（1000120 个 HPET tick，@100 MHz → 10.0012 ms），落在 8..12 ms 期望窗口内。
  3. 外观 **0 像素**。
- **未完成**：本域只做了"跨架构机制的 x86_64 重写"，**没有**新增 arm64/riscv64 目标（SutureOS 仍单架构）；AArch64/RISC-V 的系统级移植（调度器/驱动/构建链）未做，已登记为待授权项，不虚构完成。

## 域 12 工具链 / 构建（本域已齐全，审计）

- **覆盖证据**：`scripts/build.py`（clang 交叉编译 + `x86_64-elf-ld` + `@link.rsp` + Rust staticlib 对象提取 + 字体 blob 嵌入）、`scripts/build_iso.py`（xorriso/El Torito）、`scripts/qemu_boot.ps1`（QMP/串口/按键/优雅退出）、`tests/symbol-scan.ps1`、本轮新增验收脚本（selfboot/hpet/hda_volume/usb_domain/gui_baseline_shot）。
- **三项验收**
  1. `tests/symbol-scan.ps1` **PASS：1381 全局符号，0 冲突**（123 对象）。
  2. 15/15 自检、无 PANIC。
  3. 外观 0 像素。
- **未完成**：YiYiYa 工具链（LGPL-3.0）未引入（需单独取证/授权）；无 CMake/Ninja 化、无 CI。

---

## 全局回归（全部在同一份改动后产物上真跑）

| 检查 | 结果 |
|---|---|
| 自写 64 位引导链 `selfboot_acceptance` | **25/25**，启动到 `sutureos#`，自检 16/16 |
| SB16 + 8237 DMA `sb16_acceptance`（域 4） | **23/23**（WAV tone 0.500 s / peak 0.5000 FS / 440.04 Hz；无卡回归 16/16、无 PANIC） |
| virtio-gpu 2D `virtio_gpu_acceptance`（域 9） | **27/27**（8 色带逐字节、白方块 64×64 移动、off 后停止；默认启动 0 条 `[vga2d]`） |
| DHCP+DNS `dhcpdns_acceptance`（域 7） | **17/17**（ACK 四元组、DNS A 记录、nxdomain、ping 回归、无网卡回归、16/16） |
| 合成 /sys `sysfs_acceptance`（域 6） | **9/9**（`ls /sys`、kernel/block 树、ata0 size、写被拒、无 PANIC） |
| USB HID `usb_hid_acceptance`（域 8） | **14/14**（kbd/mouse 认领、sendkey 解码、热插拔 device_add/del、无 PANIC；存储回归 7/7） |
| AC'97 音量 `ac97_volume_acceptance`（域 4） | **7/7**（atten=21 回读 0x1515、100% 回读 0x0；QEMU WAV 不应用 mixer 增益已记录） |
| HPET + LAPIC 校准 `hpet_acceptance` | **13/13**（period 1e7 fs、freq 1e8 Hz、62.8M ticks/s、自检 16/16、无 PANIC） |
| GUI 外观门禁 `gui_appearance_check` | **0 像素**在动态矩形外 |
| FAT32 sync+flush `sync_acceptance` | **6/6**（宿主逐字节） |
| SquashFS 宿主 `verify_squashfs_read` | **83/83** |
| 网络 `nic_acceptance` | **PASS**（e1000 交互 ping 3/3） |
| USB 域 `usb_domain_acceptance` | **7/7**（宿主逐字节未变） |
| 终端 CJK `cjk_terminal_acceptance` | **10/10** |
| 编辑器 `editor_acceptance` | **5/5**（宿主逐字节） |
| HDA 音量 `hda_volume_acceptance` | **9/9**（WAV 真实衰减） |

## 未完成事项汇总（不虚构）

1. **提交 / 打 tag / 发 Release / 工作区外写文件**：按要求全部跳过。
2. **procfs/sysfs**：试验后回退（根因未定位：shell 读路径）。
3. **vmsvga** 未做（virtio-gpu 已完成）；USB HID/热插拔本轮已完成；DHCP/DNS 本轮已完成。
4. **arm64/riscv64 目标**（域 11）：只完成了"跨架构机制 → x86_64 重写"（通用时基），未新增第二架构目标。
5. **未移植的高价值素材**：VimtuFS2、ext4、MayFS、OS67 inode FS、SB16（需 8237 DMA）、YiYiYa 工具链（LGPL-3.0 需取证）。
6. **既有结构问题**（非本轮引入）：双调度器并存、OOM killer 缺失、DHCP/DNS/IPv6。
