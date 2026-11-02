## 🚀 本次更新 (v0.15.0 → v0.16.0)

**又搬进来三块能力：光驱里的数据盘能读（ATAPI + ISO9660）、系统第一次能出声（AC'97）、
FAT32 长文件名能创建/改名/删除（写侧）。启动呈现与 GUI 外观不变。**

> 这一版开始执行 [`docs/porting-roadmap.md`](porting-roadmap.md)（把素材库 50 个仓库里
> 可移植且有价值的东西分批搬完，每批独立验收、独立发版）。

---

### 📦 移植 1：ATAPI 光驱 + ISO9660 只读文件系统

| 项目 | 内容 |
|---|---|
| 上游 | **Uinxed-Kernel** `drivers/block/ata/pata/atapi.c`（Apache-2.0）+ **CoolPotOS** `module/iso9660`（MIT） |
| 落点 | 新增 `kernel/drivers/atapi.c/.h`（530+58 行）、`kernel/fs/iso9660.c/.h`（512+124）、`kernel/fs/iso9660_vfs.c`（357） |
| 为什么 | 此前光驱只能当启动介质，盘里的文件读不了（`ahci.c`/`block.c` 见到 ATAPI 签名直接跳过） |

- ATAPI PIO 轮询：`IDENTIFY PACKET(0xA1)` → `PACKET(0xA0)+CDB` → `READ(10)` / `READ CAPACITY` /
  `REQUEST SENSE` 兜底；注册成只读 `blk_device`（驱动号 12..15，2048B 块，READONLY|REMOVABLE）。
- ISO9660：扫**整段**卷描述符集（关键：不能停在 PVD，否则会把 El Torito 启动盘误当数据盘），
  逐段路径解析、多级目录、目录记录跨块 + 块内补齐、去 `;1`、大小写不敏感。
- 开机自动挂 `/mnt/cd`，并用 El Torito 引导记录区分"启动盘"与"数据盘"。

### 📦 移植 2：AC'97 声卡（PCM 输出）

| 项目 | 内容 |
|---|---|
| 上游 | **HBOS** `src/ac97.c` + `ac97.h`（GPL-3.0） |
| 落点 | 新增 `kernel/drivers/ac97.c/.h`（653+104 行）；kmain 挂钩；shell 新增 `beep [freq] [ms]` |
| 为什么 | 全树此前没有任何音频子系统（`AC97|HDA|audio|sound` 零命中） |

- PCI 探测（Intel 82801AA 系列 + class 0x04/0x01 通用回退）→ NAM mixer codec 复位/解静音/音量 →
  NABM **PCM OUT** 通道 32 条 Buffer Descriptor List 轮询播放，16-bit 48 kHz。
- 没有声卡时只多一行 `[ac97] no codec (skipped)`，零副作用。

### 📦 移植 3：FAT32 VFAT 长文件名（写侧）

| 项目 | 内容 |
|---|---|
| 上游 | ChaN FatFs 的 `put_lfn()` 规则（**PlantOS** MIT 血统；SpiritFoxOS 仅作算法对照） |
| 落点 | `kernel/fs/fatfs.c`（+602/−133） |
| 为什么 | v0.15.0 只做了"读长名"；创建/改名仍只能 8.3，写一个 `README.md` 都会失败 |

- 自己写 LFN 链：物理顺序 = `N|0x40 ... 1`、13 个 UTF-16 单元/条、每条重复短名校验和；
  自动生成唯一 8.3 别名（`LONG_CRE.TXT` 风格）；删除/改名时把旧链标记为已删除，不留孤儿。
- **UTF-16 → UTF-8 读回**：非 ASCII 长名（`中文 长名.txt`）被创建后能按同一串字节再打开。
- `open(create)` 原来对"还不存在的长名"根本走不到创建分支（`path_resolve` 直接 ENOENT），
  本版补上"父目录 + 长名组件"的解析路径。

---

### ✅ 验收（全部真实运行）

| 项目 | 结果 |
|---|---|
| ISO9660 数据盘 | `[iso9660] mount drive=15 vol=SUTURE_CD files=4`、`selftest PASS`；`ls /mnt/cd` 4 项、三级目录、跨 3 块大文件 4680 B、60 条目录项全部命中；宿主独立 ECMA-119 解析 **224/224** |
| 无数据盘 | 只多一行 `[iso9660] skip drive 14: El Torito boot disc, not a data disc`，无 PANIC |
| AC'97 | `[  OK  ] AC'97: Intel 82801AA (ICH) AC'97, NAM 0xc000, NABM 0xc400`；WAV 实录 **48000 Hz / 1.000 s / 0.5000 FS / 440.01 Hz（440 过零）** → 断言全 PASS |
| FAT32 长名写 | `[fat32] lfn-write self-test PASS (create=1 readdir=1 rename=1 unlink=1 utf8=1)` |
| FAT32 宿主校验 | `verify_fat32_write.py` → **21/21**（新增：无孤儿 LFN 链、无残留长名、非 ASCII 链解码回 UTF-8） |
| FAT32 读侧回归 | `lfn self-test PASS (open=1 payload=1 readdir=1)`、`ops 3/3`、`write PASS` |
| 模块自检 | `13/14`（`moos_intr` 既有项）、`syscalls: 81`、无 PANIC |
| 网络回归 | `scripts/nic_acceptance.py` 两块网卡各自 PASS（rtl8139 开机自检 reply + 交互 ping 3/3；e1000 交互 ping 3/3） |
| GUI 外观 | 新增"两次运行的活数据掩码"：`gui_appearance_check.py shot1 --live-shot shot2` → 动态矩形外 **0 像素** |
| 启动开销 | ATAPI 探测 ≤40 ms，ISO9660 automount ≤20 ms；无盘/无声卡时日志只多说明行 |

### 🧪 本版新增的验收脚本

- `scripts/mkiso9660_testcd.py` + `scripts/verify_iso9660_read.py`：造数据盘 + 宿主独立解析并逐字节比对
- `scripts/verify_ac97_wav.py`：WAV 采样率/时长/峰值/过零频率断言
- `scripts/gui_appearance_check.py` 升级：`--live-shot` 用同一构建的第二次截图学"活数据掩码"

### ⚠️ 已知限制（如实记录）

1. **ISO9660 只读**；只解析主卷（Joliet 长名 / Rock Ridge / multi-extent / UDF 未实现），
   所以文件名是大写 8.3（查找大小写不敏感）；单卷单挂、8 文件/8 目录句柄静态池。
2. **ATAPI 仅 PIO 轮询**（无 IRQ/UDMA），单条 `READ(10)` ≤8 块，只支持 2048B 块 CD-ROM；
   无换盘检测（拔盘后读会如实报 `READ(10) failed` + sense）。
3. **AC'97 单次播放 ≤2.0 秒**（静态 DMA 缓冲 480 KiB）、原生 48 kHz（其它采样率需 codec VRA）、
   纯轮询、不录音；MMIO BAR 分支未在真机验证（QEMU 只给 I/O BAR）。
4. **FAT32 长名写**：`rename` 仍是同目录（跨目录拒绝）；LFN 上限 255 UTF-16 单元 / 20 条链；
   非 ASCII 名字终端会显示为乱码（**终端没有 CJK 字体**，这属于下一批"中文支持"）。
5. About 窗口里那句 "14 upstream hobby OS projects" 属于**冻结外观的 GUI 文案**，本版没有改它；
   真实上游数量（16 个）以 README / 本文件 / `docs/provenance.md` 为准。

### 📜 出处与许可（本版新增）

- **Uinxed-Kernel — Apache-2.0**（ATAPI 参考实现）
- **CoolPotOS — MIT**（ISO9660 参考实现）
- **HBOS — GPL-3.0**（AC'97 参考实现）
- **PlantOS — MIT**（ChaN FatFs 的 LFN 规则）；SpiritFoxOS 仍无 LICENSE，本版未从其拷贝代码

### 📦 下载

- 附件 `sutureos.iso`（14366720 字节）
- sha256：`16f9365d739cf2434039fa43f055340d90037d2b29fe16c3830dd4d8b6a8d2fa`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.16.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.16.0-alpha
