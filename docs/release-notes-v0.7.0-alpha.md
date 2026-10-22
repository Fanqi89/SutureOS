## 🚀 本次更新 (v0.6.2 → v0.7.0)

第一个真正的权限模型：**用户、密码、sudoers 策略、sudo 票据、审计日志**。

### ✨ 新增

- **密码学原语**（`lib/sha256.c`）：SHA-256（FIPS 180-4）、HMAC-SHA256（RFC 2104）、
  PBKDF2-HMAC-SHA256（RFC 2898）。每次开机跑 known-answer 自检（FIPS 向量 + RFC 4231 +
  RFC 7914 风格向量）——哈希坏了等于所有密码都能过，所以这条是硬门禁。
- **用户库**（`kernel/security/userdb.c`）：解析 Linux 格式的 `/etc/passwd`、`/etc/group`，
  以及 `/etc/shadow`；首次启动自动播种账号：

  | 账号 | 密码 | uid | 说明 |
  |---|---|---|---|
  | `root` | `root` | 0 | 管理员 |
  | `user` | `user` | 1000 | 普通用户，`wheel` 组成员 |

  口令哈希存为 `$sutureos$pbkdf2-sha256$<迭代>$<盐>$<哈希>`——**刻意不用** Linux 的 crypt
  格式，因为它不是 crypt。
- **凭据**（`kernel/security/creds.c`）：按任务保存 uid/euid/suid/gid/egid（Linux 形状）。
  普通用户调 `cred_set_euid(0)` 会失败；只有 sudo 认证通过后调 `cred_escalate_root()` 才能
  提权——这就是 "setuid 位" 的等价物。
- **sudo**（`kernel/security/sudo.c` + `sudoers.c`）：
  - sudoers 语法：`user` / `%group HOST=(RUNAS:GROUP) [NOPASSWD:] CMND`，
    `Defaults timestamp_timeout` / `passwd_tries`
  - 票据缓存：认证后 15 分钟内免密，`sudo -k` 作废，`sudo -v` 刷新
  - 3 次密码尝试：`Sorry, try again.` → `sudo: 3 incorrect password attempts`
  - 两种拒绝文案（不在 sudoers / 不允许执行该命令）
  - `sudo -l` / `-u user` / `-i`
  - 每次调用往 `/var/log/sudo.log` 追加一行审计
- **shell**：`su [user]`、`whoami`、`id`、`passwd [user]`（两次隐藏输入）、`sudotest`（23 项
  验收）；提示符随身份变化（root 保持 `sutureos# `，普通用户 `user@sutureos$ `）；
  `mount`/`umount`/`shutdown`/`reboot`/`kill` 真正检查权限。

### 🛠 关键修复（都是把 sudo 挡住的老问题）

- **ramfs 单文件上限只有 64 字节**（`uint32_t data[16]`）：`/etc/motd` 一直被静默截断，
  `/etc/passwd` 根本存不下。改成 `XJ_VFS_MAX_FILE_SIZE=2048` 后用户库才可能存在。
- **快速打字丢字符**：8042 只有 1 字节输出缓冲，而 shell 每轮空闲是先清 SMP 工作队列和网卡
  RX 环形缓冲、之后才读键盘，按键会丢（实测把 `su user` 打成 `user`）。现在有 64 字节输入
  环形缓冲、一次突发读入，且键盘在其它空闲工作**之前**处理。
- PBKDF2 迭代从 10000 降到 2000：10k 每次验证约 1 秒，会把 shell 卡住、让密码提示像坏了。

### ✅ 本次验证

- `sudotest`：**23/23 PASS**（哈希、账号、shadow 落盘往返、策略、票据、凭据、审计日志）
- **QEMU 交互实测**（下面是真的串口记录）：

  ```
  sutureos# su user
  Password:
  user@sutureos$ whoami
  user
  user@sutureos$ mount fat32 0 /mnt/x
  mount: permission denied (are you root? try 'sudo mount')
  user@sutureos$ sudo -l
  [sudo] password for user:
  User user may run the following commands on sutureos:
      (ALL) ALL
  user@sutureos$ sudo mount fat32 0 /mnt/sudomnt
  [vfs] Mounted 0 on /mnt/sudomnt (fat32)          <- 真的挂载成功
  user@sutureos$ cat /var/log/sudo.log
  [484s] RUN     user=user runas=root cmd=mount fat32 0 /mnt/sudomnt
  user@sutureos$ sudo -k
  user@sutureos$ sudo whoami
  [sudo] password for user:                        <- -k 之后重新要密码
  root
  ```

  3 次错误密码会依次 `Sorry, try again.` 并最终 `sudo: 3 incorrect password attempts`；
  票据有效期内连续 `sudo` **不再**要密码。
- 回归不变：自检 13/14、FAT32 写+目录操作 PASS、宿主 17/17、`AP 1 online`、RTL8139 ping、鼠标在线

### 📦 下载与体验

- 附件 `sutureos.iso`（12.78 MiB，13,400,064 字节）
- sha256：`a38e5020ccf23c1414690de372091eedc5230d4e7e0799c4dbbe46818e9df041`
- QEMU：
  ```
  qemu-system-x86_64 -cdrom sutureos.iso -m 512 -smp 2 \
      -device rtl8139,netdev=n0 -netdev user,id=n0
  ```
- 启动后就是 root 控制台（提示符 `sutureos#`）。试这三条：
  ```
  su user          # 密码 user，提示符变 user@sutureos$
  sudo mount fat32 0 /mnt/m   # 输密码 user，真的会挂载
  sudotest         # 23 项自检
  ```

### ⚠️ 已知限制（明说，不藏）

- **这是策略层，不是安全边界**：每个任务仍在 ring 0、shell 是唯一"用户"，内核代码可以绕过
  sudo。等 ring-3 用户态落地后才是真边界；凭据 API 已经是 Linux 的 uid/euid/suid 形状，
  切换是机械工作。
- Linux 的 sudo 本体是 setuid 用户态程序，依赖 PAM/crypt(3)/termios；本系统没有用户态、
  没有 libc、只有 22 个系统调用，所以移植的是**行为契约**而不是二进制：口令用
  PBKDF2-HMAC-SHA256 而非 yescrypt，且尚无安全随机数（盐混合了 TSC、计数器与用户名）。
- 没有 `sudoedit`、没有 PAM 模块/LDAP、`Defaults` 只支持上面两项、命令匹配只到命令名
  （不含参数级粒度）。
