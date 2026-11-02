## 🚀 本次更新 (v0.27.0 → v0.28.0)

**新增 `sync`：写盘不再必须 umount。** 以前 `write` 之后要 `umount`（或再写一次）宿主才能看到数据，
因为 FAT32 的 FSInfo / ext2 的计数器只在卸载时落盘；现在 `sync` 遍历挂载表让每个驱动立即 flush。
验收里**刻意不 umount**，宿主侧解析原始镜像逐字节确认文件已在盘上。启动呈现与 GUI 外观不变。

---

### 📦 新增：`sync`

| 项目 | 内容 |
|---|---|
| 落点 | `kernel/shell/shell.c`：新增 `shell_cmd_sync()`，用 `vfs_list_mounts()` 遍历所有挂载点并逐个 `vfs_sync()`，打印每个卷的结果与 `sync: N ok, M failed` |
| 为什么 | FAT32 的 FSInfo 与 ext2 的超级块/组描述符计数是延迟写的；不卸载就直接关机/拔盘会丢数据 |
| 行为 | 逐个卷报告 `sync: <mountpoint> (<fstype>) ok|FAILED`；任一失败返回非 0 |

### ✅ 验收（真实 QEMU + 宿主逐字节，且**不 umount**）

| 项目 | 结果 |
|---|---|
| guest | `write /mnt/fat/SYNC.TXT written then synced` → `sync` → `sync: /mnt/fat (fat32) ok` + `sync: 1 ok, 0 failed` → `cat` 读回 |
| **宿主** | 解析 `build/fat32_ata.img`：`SYNC.TXT` 存在且内容 = `written then synced\n` **逐字节相同**（没有卸载动作） |
| 脚本 | `scripts/sync_acceptance.py` → **6/6 PASS** |
| 回归 | 内核自检 13/14、FAT32 宿主 21/21、GUI 外观门禁 **0 像素** |

### 🧪 本轮还做了什么（如实记录）

**尝试过把 ext2 也改成多卷（与 v0.24 FAT32 同款），但没有发布**：
- 结构性改动本身能跑通——两个 ext2 卷同时挂载、各自 `ls`/`cat`、往第二个卷写文件都成功
  （`[ext2] vfs mount drive=4 at /mnt/e2 (volume slot 1)`）；
- 但宿主侧写验证器在**卷 1** 上报 `artifact content byte-exact` 失败（`/WRT_KEEP.BIN` 首字节不符），
  说明多卷切换下写路径可能串卷或覆盖了卷 1 的块。**没有查清根因之前不回退式发布**，
  所以本批把 `ext2.c/.h`、`ext2_vfs.c` 全部 `git checkout` 回退，改交这个已验证的 `sync`。
- 该问题的现场与复现步骤已记录在 `docs/porting-roadmap.md` 的 B12 条目里，下一批继续。

### 📜 出处与许可（本版）

- `sync` 是 SutureOS 自有实现（GPL-3.0），只用公开 VFS 的挂载表与 `vfs_sync`，没有移植外部代码。

### 📦 下载

- 附件 `sutureos.iso`（15607808 字节）
- sha256：`33dfae33a45b6334d6ea6668b4e7235639c106d1862d48d394fd9bdb1ec4c61d`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.28.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.28.0-alpha
