## 🚀 本次更新 (v0.32.0 → v0.33.0)

**ISO 现在是真正的系统安装盘。** 从 ISO 启动后 shell 里跑 `install` 走终端向导：
选目标盘（二次确认）→ 可选 **root 密码**（可直接留空）→ 可选建一个普通用户（名字 + 两遍密码）→
**真实写盘**：MBR 分区表 + **Limine BIOS 引导区** + FAT32 格式化 + 复制内核/引导/配置/`/etc` →
装完**只用这张硬盘就能独立启动**，安装时设的密码/用户重启后仍然有效。

三条验收阶段全部由宿主侧独立证明（不信任客人输出）：**56/56 PASS**。

---

### 📦 新增：安装器

| 组件 | 内容 |
|---|---|
| `kernel/install/installer.c`（695 行）+ `installer.h` | 终端向导：磁盘列表（驱动器号/容量）→ 选盘 → **输入 `yes` 二次确认** → root 密码（回车留空）→ 可选普通用户（名字 + 两遍密码）→ 写盘 |
| 写盘内容 | MBR + 分区表（1 个 FAT32 分区，type 0x0C，active，起始 LBA 2048）→ **Limine BIOS stage1/stage2 按 `limine bios-install` 的等价布局写入**（stage2 在 LBA 8）→ FAT32 格式化 → 复制 `kernel.elf`、`limine.conf`、`limine-bios.sys`、`/etc/{passwd,shadow,group,sudoers,motd}`、`/SUTUREOS/INSTALLED` 标记 |
| `kernel/fs/fatfs_format.c`（231 行） | 最小 FAT32 格式化器（引导扇区 + 备份 + FSInfo + 双 FAT + 根目录簇清零 + 读回校验；簇大小自适应保证簇数 ≥ 65525） |
| `kernel/fs/fatfs.c` +173 行 | ①**分区偏移**（`start_lba`，挂载分区卷必需）；②`fatfs_mount_auto()`：先试 LBA0 超软盘（既有测试镜像行为不变），再扫 MBR 的 0x0B/0x0C/0x0E 分区；③安装写盘加速（FAT 链游标 + 整簇写 + bulk 模式跳过逐扇区回读）——原来每个 512B 扇区都从链头重走 FAT，写 2.6 MB 内核会退化成平方复杂度 |
| `kernel/main/kmain.c` +60 | **安装盘识别**：FAT32 卷里若有 `/SUTUREOS/INSTALLED`，同卷再挂 `/mnt/sys` 并把 `/etc` 五个文件播种进 ramfs、重跑 `userdb_init/sudo_init`；普通数据盘无标记时零新增挂载、零新增日志 |
| `kernel/shell/shell.c` | `install` 命令（要求 root，和 mount 同权限） |
| `scripts/installer_acceptance.py`（581 行）+ `docs/INSTALLER.md` | 三阶段验收 + 文档 |

### 🐞 顺手抓到并修掉的既有 FAT32 bug

**目录项首簇只解析了 `ent[26]`，丢了 `ent[27] << 8`** —— 首簇 ≥ 256 的文件会被读成错误的数据块。
小测试镜像簇号都 < 256，所以一直没暴露；装真系统时 `/etc/passwd` 读出来是内核机器码，才炸出来。

### ✅ 验收（三阶段，全部真跑）

| 阶段 | 内容 | 结果 |
|---|---|---|
| 1. ISO 里安装 | QMP 注入真实键盘完成 `install` 向导（选盘/确认/密码/用户） | `[install] done (drive 0, partition LBA 2048, root_password=set, user=alice)` |
| 2. 宿主解析装好的镜像 | MBR 0x55AA、分区 1 active type 0x0C LBA 2048 扇区 260096、FAT32 clusters=129020、FSInfo 三签名、**引导区与 `limine-bios-hdd.h` 的 stage1/stage2 逐字节一致**、`/kernel.elf` sha256 == `build/kernel.elf`、`/limine-bios.sys` 逐字节 == 工具链文件、`limine.conf` 有 multiboot 路径、`/SUTUREOS/INSTALLED` 在、`/etc/passwd` 有 root+alice、`/etc/shadow` 两条 PBKDF2 哈希**用 `hashlib.pbkdf2_hmac` 验证 root/rootpw 与 alice/alicepw 都 match**、`/etc/group` wheel 含 alice、`/etc/sudoers` 有 `%wheel` | 56 项全过 |
| 3. **只用硬盘启动（无 ISO）** | Limine 从装好的卷加载 `boot():/kernel.elf` → 内核 banner → `[sys] installed system volume (drive 0): seeded 5 file(s) from /mnt/sys/etc` → 终端提示符 → `cat /etc/passwd` 有 alice → **`su alice` 用安装时的密码登录成功、`whoami` = alice** | PASS |
| 空密码路径 | 另跑一遍 root 密码留空：`[install] done (... root_password=none, user=none)`，装好的盘 `/etc/shadow` = `root:!:0`（`!` = 未设密码/锁定，需 `passwd` 后再用）；启动后 `creds: 1 users (root)` | PASS |
| 回归 | 内核自检 13/14、FAT32 四自检 + 宿主 21/21、多用户 54/54、GUI 切换用户 13/13、任务栏 7/7、图片查看器 PASS、编辑器 5/5、GUI 外观门禁 **0 像素**、启动呈现前 286 行与基线逐字节相同 | PASS |

### ⚠️ 已知限制（如实记录）

1. **仅 BIOS/MBR + Limine BIOS**：装好的盘没有 UEFI/ESP、没有 GRUB；GPT 盘会被写成 MBR。
2. 只建 1 个 FAT32 分区；无 LUKS/LVM/swap/包选择/网络安装；系统文件必须来自安装 ISO。
3. ISO9660 驱动只读主卷、ISO 上的长名是 8.3 形式（`KERNEL.ELF`/`LIMINE_B.SYS`），安装器按候选名查找。
4. 最小磁盘 35 MiB；128 MiB 盘用 1 KiB 簇；安装耗时约 **60–90 s**（2.6 MB 内核 + 333 KB 引导走 FAT32 写路径）。
5. 无图形安装器（终端向导），所以不触碰冻结桌面；无安装日志落盘、无进度百分比（只有阶段行 + 每 256 KiB 一行）。
6. 多系统共存/GRUB 菜单未做；装到已有数据的盘会覆盖分区表——**向导默认不选第一块盘并要求输入 `yes` 确认**。

### 📜 出处与许可（本版）

- 安装器、FAT32 格式化器、分区挂载支持均为 SutureOS 自有实现（GPL-3.0）；Limine 引导区按工具链自带的
  `limine-bios-hdd.h` 布局写入（Limine 为 BSD-2-Clause，见 `docs/provenance.md`），没有移植外部安装器代码。

### 📦 下载

- 附件 `sutureos.iso`（15939584 字节）
- sha256：`a7b7b0322a0a75dd63e0e9e642d94238115fc10baf448be1e92658fddf39863c`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.33.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.33.0-alpha
