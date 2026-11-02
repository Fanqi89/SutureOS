## 🚀 本次更新 (v0.23.0 → v0.24.0)

**FAT32 驱动支持多卷：IDE 盘挂在 `/mnt/fat`、U 盘挂在 `/mnt/usb`，同一次启动里两个卷同时可读可写
（这是 v0.23.0 里如实标注的"fatfs 单卷"限制的移除）。宿主侧解析两个镜像逐字节验证：写 U 盘只改
U 盘，IDE 卷内容不变。启动呈现与 GUI 外观不变。**

---

### 📦 改动：FAT32 从"单卷"变成"每挂载点一份状态"

| 项目 | 内容 |
|---|---|
| `kernel/fs/fatfs.c` | `g_vol` → `g_vols[4]` + 当前卷指针 `g_vol`（98 处引用零改动）；新增 `fatfs_select_volume/fatfs_current_volume/fatfs_volume_count`；`fatfs_mount()` 现在返回**卷槽号**；`fatfs_unmount(drive)` 按驱动器清对应槽并关掉该卷的文件/目录句柄；文件与目录句柄各自记住 `vol`，每次读写前自动切回自己的卷 |
| `kernel/fs/fatfs_vfs.c` | 挂载点表 `g_mnts[4]`（mountpoint / drive / vol）；`vol_path()` 按 mounts 做前缀匹配；每个 VFS 操作先 `fatfs_select_volume(m->vol)`；`unmount` 按挂载点找槽（不再假设只有一个卷） |
| `kernel/main/kmain.c` | USB 挂载段的注释与 EBUSY 文案更新（EBUSY 现在是"卷表满/驱动占用"，不再是"fatfs 只有一卷"） |

### ✅ 验收（真实 QEMU + 宿主逐字节）

| 项目 | 结果 |
|---|---|
| 双卷同挂 | `[init] FAT32 volume auto-mounted at /mnt/fat` + `[usb] mount /mnt/usb OK` 同一次启动 |
| 各自列举 | `ls /mnt/fat` → HELLO.TXT/README.TXT/…/`中文 长名.txt`（中文仍正常渲染）；`ls /mnt/usb` → USB.TXT/HELLO.TXT/README.TXT/`Long file name test.txt` |
| 各自读取 | `cat /mnt/fat/hello.txt` 与 `cat /mnt/usb/USB.TXT` 分别返回各自卷的内容（不再串卷） |
| 交叉写 | 在 `/mnt/fat` 挂着的情况下 `write /mnt/usb/mv.txt hi both volumes` → 成功（16 字节）→ `umount /mnt/usb` |
| **宿主验证** | U 盘镜像里 `MV.TXT` 内容逐字节等于写入内容（`hi both volumes\n`）；IDE 卷的 `HELLO.TXT` 仍逐字节可读（USB 写没有污染 IDE 卷）→ `scripts/multivol_acceptance.py` **12/12 PASS** |
| 单卷回归 | 只挂 IDE 盘：内核自检 13/14、FAT32 写/ops(3/3)/长名/长名写四自检 PASS、宿主 `verify_fat32_write.py` **21/21**；只挂 U 盘：`verify_usb_disk.py` 5/5 |
| 其它回归 | 终端中文 **10/10**（双卷路径下中文长名正常）、wget **8/8**、无 PANIC、GUI 外观门禁 **0 像素** |

### 🧪 新增脚本

- `scripts/multivol_acceptance.py`：一次启动挂两个 FAT32 卷，逐项断言（列举/读取/交叉写/umount），
  并**在宿主侧解析两个镜像**证明写入落在正确卷、另一卷内容未变。

### ⚠️ 已知限制（如实记录）

1. 卷槽上限 4（`FATFS_MAX_VOLS`，mounts 表同样 4）；超过时报 EBUSY 而不是静默替换。
2. 路径归属用"挂载点最长前缀匹配"，因此**挂载点不能互相嵌套**（例如 `/mnt` 与 `/mnt/fat` 同时挂
   FAT32 会有歧义，实测只按最长前缀走）。
3. 文件/目录句柄表仍是全局 8+8（跨卷共享），同时打开超过 8 个文件会失败（既有行为）。
4. ext2/squashfs 适配层仍是单卷（本批只改 FAT32）；`mount` 命令挂同类型第二个卷时按各自驱动能力行事。
5. 多卷写入的并发保护没有变化（内核仍是单线程 shell 语义），没有新增锁。

### 📜 出处与许可（本版）

- 多卷改造是 SutureOS 自有实现（GPL-3.0），只动 FAT32 驱动与 VFS 适配层，没有移植外部代码。

### 📦 下载

- 附件 `sutureos.iso`（15464448 字节）
- sha256：`b1e75d37e48e5115ccfde64acb09becb894f145f6a68525987fa24f76781cc68`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.24.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.24.0-alpha
