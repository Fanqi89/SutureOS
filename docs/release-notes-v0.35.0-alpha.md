## 🚀 本次更新 (v0.34.0 → v0.35.0)

**数据持久化**：装到硬盘后 `/etc`、`/home`、`/root` 双向镜像到系统卷的 `/SUTUREOS/PERSIST`，
**重启/关机不再丢数据**——安装时设的用户/密码、useradd 出来的账号、写进家目录的文件、改过的 `/etc`
关机后再开机全部原样。

---

### 📦 持久化布局与触发点

| 项目 | 内容 |
|---|---|
| 卷上布局 | `<系统卷>/SUTUREOS/PERSIST/{VERSION,etc/,home/,root/}`，`VERSION` = `sutureos-persist 1` |
| 开机恢复 | `kmain` 优先从 `PERSIST` 恢复（`[persist] restored N file(s)`），没有 PERSIST 的旧卷退回原来的 `/mnt/sys/etc` 播种；恢复后重建 `/home`、`/root`、每个 uid≥1000 账户的 `/home/<name>`，再跑 `userdb_init/sudo_init` |
| 落盘时机 | `sync` 先 flush；`shutdown`/`reboot` 先 flush + 所有挂载点 sync 再 ACPI S5/复位；退出 GUI 回终端时 flush；新增 `persist [flush]` 命令手动触发 |
| 实现 | `kernel/fs/persist.c`(244)/`persist.h`(44)：VFS 递归 walk 双向镜像，深度≤6、每目录≤64 项、每次≤256 文件、单文件≤128 KiB；flush 末尾 `vfs_sync("/mnt/sys")` 保证 FAT/FSInfo 落盘；无系统卷时全部 no-op（live ISO 零新增日志） |
| 安装器 | 安装时写 `PERSIST` 骨架，并把新卷挂到 `/mnt/sys` + `persist_detect()` —— **同一次 live 会话的 `sync`/`shutdown` 就能落盘** |

### ✅ 验收（三阶段，真 QEMU + 宿主独立解析）

| 阶段 | 证据 |
|---|---|
| 1. 安装 + 改数据 | `[install] writing /SUTUREOS/PERSIST skeleton` → `persistence active for this session` → `useradd: created bob uid=1001` → 写 `/home/bob/NOTE.TXT`、改 `/etc/motd` → `sync` 打 `[persist] flush 6 files` → `shutdown` 再 flush 一次并 **ACPI S5 真关机** |
| 2. 宿主解析装好的盘 | `PERSIST/etc/motd` = `changed by user`；`PERSIST/home/bob/NOTE.TXT` = `hello persistence`(18B)；`PERSIST/etc/shadow` 里 bob 的 `$sutureos$pbkdf2-sha256$2000$…` 用 `hashlib.pbkdf2_hmac` 重算 **match=True**；`PERSIST/etc/passwd` root+bob；`VERSION` 19B |
| 3. 只挂该盘重启（无 ISO） | `[persist] restored 6 file(s) from /mnt/sys/SUTUREOS/PERSIST` → `cat /etc/motd` 是改后文本 → `users` 有 bob → `cat /home/bob/NOTE.TXT` 正确 → **`su bob` 用重启前设的密码成功、`whoami`=bob** → `persist` 报 `format=v1 / restored at boot=6 file(s)` |
| 脚本 | `scripts/persistence_acceptance.py` → **32/32 PASS** |
| 回归 | 安装器 **56/56**、内核 13/14、FAT32 21/21、应用注册表/任务栏 9/9、多用户 54/54、GUI 外观门禁 0 像素、启动呈现前 297 行与基线逐字节相同 |

### ⚠️ 已知限制（如实记录）

1. 镜像是"复制"不是"同步删除"：在 ramfs 里删掉的文件不会从卷上删，下次开机会被恢复回来（只保证新增/覆盖类改动持久）。
2. 有界：深≤6、每目录≤64 项、每次≤256 文件、单文件≤128 KiB（超出静默跳过）。
3. 无权限/属主元数据（VFS 没有所有权）；`/etc` 保护仍是 shell/cred 策略层。
4. 非原子写：flush 中途硬断电可能留半截文件（正常 shutdown/reboot 会先 flush）。
5. 只镜像第一个 FAT32 系统卷（`/mnt/sys`），第二块盘不参与；每次全量重写镜像文件（小数据可忽略）。
6. 顺带观察（既有 ATA 代码）：主机很忙时偶发 `[blk] ata flush timeout`，数据仍落盘、宿主校验通过，未改 ATA 驱动。

### 📜 出处与许可（本版）

- 持久化模块、安装器改动均为 SutureOS 自有实现（GPL-3.0），没有移植外部代码。

### 📦 下载

- 附件 `sutureos.iso`（16001024 字节）
- sha256：`dad5958b835541a5b82ff7a664a68d85a1a00e74ce03072a3f393e5ed1f6af81`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.35.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.35.0-alpha
