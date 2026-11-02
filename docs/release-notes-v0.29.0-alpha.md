## 🚀 本次更新 (v0.28.0 → v0.29.0)

**EXT2 支持多卷：一次启动里可以同时挂两个（甚至更多）ext2 卷，各自独立读写。
上一轮如实标注的"多卷串卷"问题已经定位根因并修好——宿主验证器现在同时证明
"卷 2 拿到了写入、卷 1 一字未变"。启动呈现与 GUI 外观不变。**

---

### 🐞 上一轮到底哪儿坏了（现象 → 根因 → 怎么修）

| 现象 | 根因 | 修法 |
|---|---|---|
| 挂上第二个 ext2 卷并写入后，宿主写验证器在**卷 1** 报 `artifact content byte-exact` 失败（`/WRT_KEEP.BIN` 首字节不符） | 多卷改造给 ext2 的**只读 open** 路径记了 `f->vol`，但 **O_CREAT 的写句柄**（`ext2_file_open`，另一处 `f->used = 1;`）漏了：句柄 `vol` 恒为 0 → 所有写操作都切到**第一个卷**，于是"写 /mnt/e2 的 TWO.TXT"实际写进了卷 1 的盘，覆盖了它的 `WRT_KEEP.BIN` 数据块 | 两处句柄认领点都记录 `f->vol = g_cur_ext2_vol`（目录句柄同样处理）；验收里加了"卷 1 镜像里**不允许**出现卷 2 的文本"这条硬断言，防止同类回归 |

这也解释了两轮里"目录能读、文件写串"的不对称现象：读走 `ext2_open`（有 vol），写走 `ext2_file_open`（漏了 vol）。

### 📦 新增：EXT2 多卷

| 项目 | 内容 |
|---|---|
| `kernel/fs/ext2.c` | 单卷 `struct ext2_fs g_ext2_fs` → `g_ext2_fss[4]` + 当前卷指针 `g_ext2_fs`；新增 `ext2_select_volume / current_volume / volume_count / unmount_drive`；`ext2_mount()`、`ext2_mount_ramdisk()` 返回**卷槽号**；文件/目录句柄各带 `vol`，每次操作前切回自己的卷；卸载按驱动器清槽并关掉该卷句柄 |
| `kernel/fs/ext2.h` | 多卷 API 与句柄字段声明 |
| `kernel/fs/ext2_vfs.c` | 挂载点表 `g_ext2_mnts[4]`（mountpoint/drive/ram/vol）+ 每个 VFS 操作先选卷；`unmount` 按挂载点定位；VFS 写自检改用挂载点表（不再假设只有一个 "/mnt/ext2"） |

### ✅ 验收（真实 QEMU + 宿主双镜像逐字节）

| 项目 | 结果 |
|---|---|
| 挂载 | 开机自动挂卷 0（`/mnt/ext2`）；shell `mount ext2 4 /mnt/e2` → `[ext2] vfs mount drive=4 at /mnt/e2 (volume slot 1)`（第二块盘走 AHCI drive 4——SutureOS 的 ATA 只注册 drive 0/1） |
| 独立访问 | 两卷各自 `ls`/`cat` 正确，卷 1 仍带着自检产物 `WRT_KEEP.BIN` |
| 写卷 2 | `write /mnt/e2/TWO.TXT hello from volume two` → `cat` 读回 → `umount /mnt/e2` |
| **宿主** | 卷 1 通过完整 ext2 写验证器 **40/40**（自检产物完好）；卷 2 fixture 通过只读验证 **35/35**；卷 2 镜像确实变化（48 字节）且包含写入文本；**卷 1 镜像里不含卷 2 的文本** |
| 脚本 | `scripts/multivol_ext2_acceptance.py` → **12/12 PASS** |
| 回归 | 内核自检 13/14、FAT32 宿主 21/21、无 PANIC、GUI 外观门禁 **0 像素** |

### ⚠️ 已知限制（如实记录）

1. 卷槽上限 4（`EXT2_MAX_VOLS`，挂载点表同样 4），超出如实报 EBUSY。
2. 挂载点用最长前缀匹配，**不能互相嵌套**；文件/目录句柄表仍全局 8+8。
3. 仍只支持 ext2（无 ext4 extents/64bit/元数据校验和），三间接写如实拒绝（既有行为）。
4. squashfs 适配层**仍是单卷**（本批只改 ext2），下一批处理。
5. 挂载第二个 ext2 卷需要它出现在另一个控制器上（AHCI/NVMe/VirtIO），因为 SutureOS 的 ATA 驱动只注册 drive 0/1。

### 📜 出处与许可（本版）

- 多卷改造是 SutureOS 自有实现（GPL-3.0），只动 ext2 驱动与 VFS 适配层，没有移植外部代码。

### 📦 下载

- 附件 `sutureos.iso`（15632384 字节）
- sha256：`8292c1347fdd1d841905aa16a097faadae3104d27aaad2dd9462459453c8a895`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.29.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.29.0-alpha
