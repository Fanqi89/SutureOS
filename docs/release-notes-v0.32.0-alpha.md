## 🚀 本次更新 (v0.31.0 → v0.32.0)

**多用户体系（用户三件套之一）**：`useradd` / `userdel` / `users` / `passwd -l/-u/-d` / `login` / `su -`
全部打通，GUI 开始菜单新增 **Switch User** 对话框，切换后任务栏的当前用户立即更新；
`/etc`、`/root` 对非 root 写操作有策略保护。验收脚本 **54/54**（多用户 shell）+ **13/13**（GUI 切换），
宿主侧用 `hashlib.pbkdf2_hmac` 独立验证密码哈希。

---

### 🔑 密码"三态"语义（明确判定，不是漏洞）

| `/etc/shadow` 密码字段 | 语义 | 谁在用 |
|---|---|---|
| `$sutureos$pbkdf2-sha256$<iter>$<salt>$<hash>` | **需要密码** | `useradd` 设了密码、`passwd` 改密码 |
| **空字段**（`name::0:...`） | **免密登录** | **安装器留空 root 密码时写这个**（`passwd -d` 也可得到） |
| `!` / `*` | **锁定** | `useradd` 留空 → 禁用密码；`passwd -l` |
| 没有 shadow 行 | 锁定 | 异常/被删用户 |

判定入口只有一个 `userdb_pwstate()`；`sudo` 对**免密账户仍拒绝**（"没有密码可证"，刻意如此并写进
`docs/multiuser.md`）。

### 📦 新增命令与能力

| 命令 | 行为 |
|---|---|
| `useradd <name>` | 建用户（uid ≥1000、私有组、`/home/<name>`），交互设密码（留空=锁定），名字校验、重名拒绝 |
| `userdel <name>` | 删除用户（**拒绝 root / 拒绝删当前用户**），一次性重写 passwd+shadow+group |
| `users` | 列出 用户名/uid/gid/组/密码状态（hash / none / locked） |
| `passwd [-l|-u|-d] [user]` | root 可改任意人；普通用户改自己要先验旧密码；`-l` 锁定、`-u` 解锁、`-d` 清空=免密 |
| `login [user]` | 输密码切换身份（免密账户直接放行、锁定账户明确拒绝 `account is locked`） |
| `su - [user]` | 走同一套 `login` 引擎：清 sudo 票据、刷新提示符、`su -` 切到 home |
| GUI **Switch User** | 开始菜单新增（第 6 项，懒创建自绘对话框），成功后任务栏当前用户立即变 |

### 🛡 身份隔离（策略层，如实标注）

- 原有硬策略不变：`mount/umount/shutdown/reboot/kill` 要求 euid=0。
- 新增 shell 层：非 root 的 `write/append/rm/mkdir/rmdir/touch/mv` 以及 `cp` 的目标若落在 `/etc` 或
  `/root` → `Permission denied (only root may modify /etc or /root)`；**读仍然允许**（`cat /etc/passwd`、
  `cp /etc/passwd /mnt/fat/...` 可跑，正好用来做宿主校验）。
- 边界：VFS 没有 per-file 所有权/权限位，所有任务仍在 ring 0，所以这是**策略层不是硬件边界**（文档写明）。

### ✅ 验收（真 QEMU + QMP 注入 + 宿主独立校验）

| 项目 | 结果 |
|---|---|
| 多用户 shell | `useradd alice`（uid=1001，私有组）→ `cat /etc/shadow` 看到哈希 → `su alice` 输密码成功（提示符变 `alice@sutureos$`）→ 错误密码被拒且身份不变 → `login` 错误密码 `Login incorrect` → 锁定账户被拒 → `passwd -d` 后免密 → `userdel` 拒绝 root、删除 alice/carol 成功 |
| sudo 回归 | `sudotest` **23/23**；普通用户 `sudo -l` 被拒、wheel 成员可用 |
| **宿主校验** | 客人把 `/etc/passwd`、`/etc/shadow`、`/etc/group` 三份快照复制到 FAT32 → 宿主解析原始镜像：字段数/uid 唯一/≥1000/私有组一致；**用 `hashlib.pbkdf2_hmac` 按 shadow 里的 salt+iterations 重算，与 alice 的哈希逐字节相同**（root/user 哈希经四次重写仍可验证）；删除后两文件无残留 |
| GUI 切换 | 真鼠标点开始菜单 `Switch User`（8 项菜单新几何）→ 真按键输入 → `[login] gui switch -> user (uid=1000)`；**默认桌面像素与基线完全一致（0 像素）** |
| 脚本 | `scripts/multiuser_acceptance.py` **54/54**、`scripts/switch_user_acceptance.py` **13/13** |
| 回归 | 内核自检 13/14、FAT32 宿主 21/21、任务栏 7/7、图片查看器 PASS、文本编辑器 5/5 |

### ⚠️ 已知限制（如实记录）

1. 无 PAM / login.defs / 密码时效 / groupadd / usermod；`userdel` 不删 home；用户与组上限 16。
2. 盐来自 `userdb_new_salt()`（TSC+计数器经 SHA-256 混合），**不是 CSPRNG**。
3. `/etc` 保护是 shell 策略层（见上），VFS 无 per-file 所有权。
4. GUI 切换对话框只支持键盘（Enter 换字段/提交、Backspace 编辑）——Tab 被合成器用作窗口焦点、Esc 退出桌面。
5. 开始菜单从 7 项变 8 项：`imgview_acceptance.py` / `editor_acceptance.py` 的几何常量已同步更新，两个脚本实跑通过；默认桌面外观门禁仍 0 像素（菜单默认关闭）。

### 📜 出处与许可（本版）

- 多用户体系是 SutureOS 自有实现（GPL-3.0），复用已有 userdb/creds/sudo 与 GUI 绘图 API，没有移植外部代码。

### 📦 下载

- 附件 `sutureos.iso`（15800320 字节）
- sha256：`3b0c8d3e73e9fb617abf1e053b6af46683c9396b29ab40d7d850e1be67ea197d`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.32.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.32.0-alpha
