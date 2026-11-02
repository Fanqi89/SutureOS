## 🚀 本次更新 (v0.29.0 → v0.30.0)

**SquashFS 也支持多卷了**——至此 FAT32 / ext2 / squashfs 三种真实文件系统都做到"一次启动里多个卷
同时挂载、各自独立可用"。本批收尾时跑了**全套回归**（见下表）。启动呈现与 GUI 外观不变。

---

### 📦 新增：SquashFS 多卷

| 项目 | 内容 |
|---|---|
| `kernel/fs/squashfs.c` | `g_squashfs_fs` / `g_squashfs_mount` 单例 → `[SQUASHFS_MAX_VOLS=4]` 数组 + 当前卷指针；新增 `squashfs_select_volume / current_volume / volume_count / unmount_drive`；`squashfs_mount()`、`squashfs_mount_ramdisk()` 返回**卷槽号**；文件/目录句柄各带 `vol`，**所有**认领点都记录（v0.29 的教训：漏一处就会写/读串卷） |
| `kernel/fs/squashfs.h` | 多卷 API 与句柄字段 |
| `kernel/fs/squashfs_vfs.c` | 挂载点表 `g_squash_mnts[4]` + 每个 VFS 操作先选卷；unmount 按挂载点定位；只读语义不变（写操作仍如实返回 EPERM 并只记一次日志） |

### ✅ 本批验收（真实 QEMU + 宿主校验）

| 项目 | 结果 |
|---|---|
| 挂载 | 开机挂卷 0（`/mnt/squash`）；shell `mount squashfs 4 /mnt/s2` → `[squashfs] vfs mount drive=4 at /mnt/s2 (volume slot 1)` |
| 独立访问 | 两卷各自 `ls`/`cat` 正确（第一仍是 `LARGE.BIN`/`FRAG.TXT` 那套夹具） |
| **宿主** | 两个镜像各自通过 squashfs 只读验证器 **83/83**（只读文件系统，镜像逐字节未变） |
| 脚本 | `scripts/multivol_squashfs_acceptance.py` → **7/7 PASS** |

### 🧪 收尾全量回归（同一份 v0.30.0 ISO）

| 门禁 | 结果 |
|---|---|
| 内核自检 | **13/14**（`moos_intr` 是长期既有判定） |
| FAT32 宿主写验证 | **21/21**，四个内核 FAT32 自检（write/ops/lfn/lfn-write）全 PASS |
| EXT2 多卷 | **12/12**（卷 1 写验证器 40/40、卷 2 fixture 35/35、无串卷） |
| SquashFS 多卷 | **7/7**（两镜像各 83/83） |
| USB 2.0 存储 | 只读 5/5、写路径 8/8（宿主双镜像） |
| Intel HDA | 宿主 WAV **6/6**（48000 Hz/2ch/16bit/1.002 s/0.5000 FS/440.01 Hz） |
| 终端 CJK | **10/10**（`ls` 中文名 + 滚动后逐像素精确） |
| GUI CJK | **4/4**（图片查看器“缩放”字形逐像素） |
| wget | **8/8**（真实 HTTP 服务器 + 宿主 FAT32 逐字节） |
| 文本工具 | **7/7**（wc/head/tail/grep/stat） |
| 文本编辑器 | **5/5**（真实点击+键盘 → 宿主 FAT32 逐字节） |
| sync | **6/6**（不 umount 落盘） |
| GUI 外观门禁 | 动态矩形外 **0 像素** |

### ⚠️ 已知限制（如实记录）

1. 三种文件系统的卷槽都是 4；挂载点用最长前缀匹配、**不能互相嵌套**；文件/目录句柄表仍全局 8+8。
2. squashfs 仍是**只读**（写操作如实返回 EPERM）；不支持 xz/zstd/lzo、xattr、export 表、目录索引项、
   >4 KiB 数据块（既有边界）。
3. 第二个卷通常要挂在 AHCI/NVMe/VirtIO 上：SutureOS 的 ATA 驱动只注册 drive 0/1。
4. 仍未做：USB 热插拔重扫 / EHCI HID 键盘、virtio-gpu 的 2D 呈现增强（两项都在路线图里标为待做）。

### 📜 出处与许可（本版）

- squashfs 多卷是 SutureOS 自有改动（GPL-3.0），基于已移植的 squashfs 驱动（源自 collection 内
  许可合规实现，出处见 `docs/provenance.md`），本批没有新增第三方代码。

### 📦 下载

- 附件 `sutureos.iso`（15644672 字节）
- sha256：`5d6ae3f61414eb2ee241ccb6f7752e8db445d6ee9bc4059a992bba1721374b16`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.30.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.30.0-alpha
