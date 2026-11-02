## 🚀 本次更新 (v0.22.0 → v0.23.0)

**SutureOS 第一次把 U 盘当盘用：EHCI（USB 2.0）主控 + 端口复位 + 高速枚举 + BOT/SCSI
（INQUIRY / READ CAPACITY / READ(10) / WRITE(10) / SYNCHRONIZE CACHE）→ 注册成块设备 drive 16 →
FAT32 自动挂到 `/mnt/usb`，能 `ls` / `cat` / `write`，宿主侧解析镜像逐字节验证。
启动呈现与 GUI 外观不变。**

---

### 📦 新增：USB 2.0 主机控制器 + 大容量存储

| 项目 | 内容 |
|---|---|
| 文件 | `kernel/drivers/usb_ehci.c`（1430 行，新）、`usb_ehci.h`（64 行，新）、`block.h`（+3：`BLK_TYPE_USB`/`BLK_DRIVE_USB=16`）、`kmain.c`（+44：`usb_probe()` + `/mnt/usb` 挂载 + 自检） |
| 来源 | 骨架 **HanOS `kernel/device/usb/ehci.c`（MIT）**；CBW/CSW 与 BOT 流程对照 **HBOS `src/usb_msc.c`（GPL-3.0）**；SutureOS 侧改动逐条写在文件头注释（PCI 机制#1、`mmu_map_mmio_identity`、PIT 延时、静态 .bss DMA、新增 TUR/INQUIRY/SYNC CACHE 与 blk 层接入） |
| 能力 | 端口 Reset（写 PR=1 保持 ~50ms → 显式清 PR → 轮询 PED）、高速枚举（SET_ADDRESS / GET_DESCRIPTOR / SET_CONFIGURATION）、异步 QH/qTD 链、BOT 三个阶段、SCSI READ(10)/WRITE(10)/READ CAPACITY(10)/INQUIRY/TEST UNIT READY/SYNCHRONIZE CACHE、`blk_register`（drive 16，512 B 块，读写） |
| 工具 | `scripts/mk_usb_testimg.py`（71680×512 的 FAT32 夹具，含 USB.TXT/HELLO.TXT/README.TXT）、`scripts/verify_usb_disk.py`（宿主侧独立校验，205 行） |

### ✅ 验收（真实 QEMU 11.1 + 宿主侧独立校验）

| 项目 | 结果 |
|---|---|
| 有 USB 盘 | `[usb] ehci: pci 0:4.0 mmio=febf1000 ports=6 caplen=32` → `port 1 reset ok speed=high (portsc=1005)` → `device addr=1 mps=64 vendor=46f4 product=0001` → `inquiry vendor=QEMU product=QEMU HARDDISK` → `storage: 71680 blocks x 512 (drive 16, rw)` → `mount /mnt/usb OK` → `selftest PASS (read=1 cat=1)` |
| shell 实测 | `ls /mnt/usb` → `USB.TXT / HELLO.TXT / README.TXT / Long file name test.txt`；`df` → `fat32 16 35272 2 35269 /mnt/usb` |
| 写路径 | `write /mnt/usb/w.txt hi usb write 42`（16 字节）→ `cat` 读回相同 → `umount /mnt/usb` → 宿主侧 **6 个扇区变化且全落在 FAT/FSInfo/根目录/新簇**、`W.TXT` 16 字节逐字节正确、原有文件一字节未变、`FAT1==FAT2`、FSInfo 复算一致 → `verify_usb_disk.py` **8/8 PASS** |
| 只读启动 | 自检内部"写→读回→还原"后整盘 **CRC32 93F4BB22 一字未变** → 5/5 PASS（证明写路径没有越界写） |
| 无 USB 启动 | 只多一行 `[usb] no controller`；内核自检 13/14、FAT32 写/ops(3/3)/长名/长名写四自检 PASS、无 PANIC、FAT32 宿主验收 21/21 |
| GUI 外观 | `gui_appearance_check.py` 动态矩形外 **0 像素** |

### 🧪 新增脚本

- `scripts/mk_usb_testimg.py`：确定性 USB 测试盘夹具。
- `scripts/verify_usb_disk.py`：宿主侧独立校验（FAT 几何 / FAT1==FAT2 / FSInfo 复算 / 逐文件字节 /
  `--allow-write` 时逐扇区差异必须落在合法区域内 / `--expect-file` 断言 shell 写入的文件内容）。

### ⚠️ 已知限制（如实记录，别当已验）

1. **只做高速设备**：全速/低速不做 split transaction，复位后 PED 未置位就把端口交还伴随控制器并打日志。
2. QEMU 的 EHCI 模型不填 `PORTSC.PSPD`（恒 0），所以"是否高速"用规范判据：复位释放后 `PED=1`。
3. **fatfs 只有单卷**：同时挂 `-hda` FAT32 盘时 `/mnt/fat` 先占卷，`/mnt/usb` 会打印
   `mount /mnt/usb skipped: fatfs keeps one volume ...` 并如实 `selftest FAIL`（不崩、不阻塞启动）；
   要两个卷同时可读需把 fatfs 改多卷（不在本批）。
4. 只认第一台 mass storage、LUN 0；**仅开机枚举，无热插拔重扫**；无 EHCI HID 键盘；
   BOT 数据阶段 16 KiB 分块。
5. USB 事务 CRC / 真机错误路径**未验**（QEMU 没有对应模型），只有有界超时与 `[usb]` 日志。
6. shell `write` 不 flush FSInfo，需要 `umount`/`vfs_sync` 后宿主才看到一致（既有 FAT32 行为）。

### 📜 出处与许可（本版）

- `kernel/drivers/usb_ehci.c`：骨架 **HanOS（MIT）** 的 EHCI 驱动，BOT/SCSI 流程对照 **HBOS（GPL-3.0）**，
  SutureOS 侧改动（PCI/MMIO 映射/延时/静态 DMA/blk 接入/新增命令）逐条记在文件头；
  两者均可并入 GPL-3.0 项目。README 的 About 窗口"upstream projects" 计数属冻结外观，未改；
  真实上游清单一律以 `docs/provenance.md` / `docs/licensing-audit.md` 为准。

### 📦 下载

- 附件 `sutureos.iso`（15448064 字节）
- sha256：`8e4366d30342a7397d940c8829bfdbf2b0c7342ba35510ae5eb73e1c0b4f6808`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.23.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.23.0-alpha
