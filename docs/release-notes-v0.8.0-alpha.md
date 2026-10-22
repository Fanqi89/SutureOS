## 🚀 本次更新 (v0.7.0 → v0.8.0)

把审计里「属于 **ring 0** 的未完成项」做掉：系统调用面、真实墙钟、管道、文件管理命令、信号投递。
用户态（ring 3）留给下一批。

### ✨ 新增

**系统调用面 22 → 56 个**

| 类别 | 新增的调用 |
|---|---|
| 文件状态/偏移 | `stat` `fstat` `lstat` `access` `lseek` `ioctl` `statfs` |
| 目录与路径 | `chdir` `getcwd` `mkdir` `rmdir` `unlink` `rename` `getdents64` |
| 写入同步 | `fsync` `fdatasync` `sync` `truncate` |
| 身份 | `getpid` `gettid` `getppid` `getuid` `geteuid` `getgid` `getegid` `setuid` `setgid` `uname` |
| 时间 | `gettimeofday` `time` `clock_gettime`（真时钟）`nanosleep` `times` |
| 进程/信号 | `kill` `wait4` `exit_group` |
| 系统信息 | `sysinfo` `getrlimit` `setrlimit` `prctl` `sched_getaffinity` `getcpu` |
| IPC | `pipe` `pipe2` |

`stat` 填的是 **Linux 逐字节一致的 144 字节 `struct stat`**；`uname` 返回
`sysname="Linux"`、`release="6.1.0-sutureos"`（libc 的探测能过）；相对路径按 cwd 解析。

- **CMOS RTC 墙钟**（`arch/x86_64/rtc.c`）：以前 `clock_gettime` 返回写死的
  `1234567890`、没有 `date`。现在开机读一次 RTC（BCD/二进制、12/24 小时制、century
  寄存器），把单调 tick 锚到真实时间上。
- **管道**（`kernel/ipc/pipe.c`）：管道 fd 有自己的号段，`read`/`write`/`close` 先分流到它；
  写端全关后读到 EOF、无读端写返回 `-EPIPE`、满时短写、空时有界等待。
- **shell**：`rm` `mkdir` `rmdir` `touch` `cp` `mv` `date` `uname` `free`，
  外加三个验收命令 `systest` / `pipetest` / `rtctest`。
- **信号投递**：`sig_check_pending()` 以前**零调用点**，现在挂在 syscall 返回路径
  （仅用户任务 `cs=0x23` 时），`kill(2)` 会把信号真的投进去。

### 🛠 关键修复

- **ramfs 的 `rename` 是桩**（直接 `return VFS_EPERM`）→ 实现同目录改名
  （`mv a b` 需要的正是这个），带 EEXIST/EXDEV 检查。第一次跑 `systest` 就是这条 FAIL。
- **RTC 12 小时制解码错**：PM 位（bit 7）没剥掉就参与换算，"1 PM" 变成 141 而不是 13。
- **RTC 初始化把年份按 BCD 解了两次**（传进去的已是二进制年），于是每次读都
  "implausible"、`date` 停在 1970-01-01。
- **`date` 补零**：本内核 `snprintf` 没有 `0` 标志，改为手工补位
  （`2026-10-22 23:04:11 UTC`，而不是 `2026-10- 1  1: 4:11`）。
- 自检里用了 `%-43s`，而 `console_printf` 的格式化器没有 `-` 标志、会原样打印 → 改普通 `%s`。

### ✅ 本次验证

- `systest`：**39/39 PASS** — stat/lseek/read/write/fstat、mkdir/getdents64/unlink/rmdir、
  rename、chdir/getcwd、getpid/getuid/geteuid、uname、真实 clock_gettime/gettimeofday/time、
  管道走系统调用、sysinfo、getrlimit、sync，以及诚实的 `-ENOSYS`/`-ENOTTY` 回答
- `pipetest`：**16/16 PASS**；`rtctest`：**8/8 PASS**（BCD/二进制/12 小时/闰日/格式化）
- 交互实测（串口原样）：

  ```
  sutureos# date
  2026-10-22 23:06:17 UTC
  sutureos# mkdir /demo
  sutureos# touch /demo/a.txt
  sutureos# cp /etc/motd /demo/b.txt
  cp: /etc/motd -> /demo/b.txt (92 bytes)
  sutureos# ls /demo
  a.txt
  b.txt
  sutureos# mv /demo/b.txt /demo/c.txt
  sutureos# ls /demo
  a.txt
  c.txt
  sutureos# free
  Mem:      125168 KiB     8240 KiB   116928 KiB
  sutureos# uname -a
  Linux sutureos 6.1.0-sutureos SutureOS 0.8.0-alpha x86_64 (none)
  ```

- 回归不变：自检 13/14、FAT32 写+目录操作 PASS、宿主 17/17、`AP 1 online`、
  RTL8139 ping、PS/2 鼠标、sudo 23/23

### 📦 下载与体验

- 附件 `sutureos.iso`（12.97 MiB，13,596,672 字节）
- sha256：`0c33e9759a1a41289002f503f3800fb14c824626ed233f321a8a173b870663dd`
- QEMU：
  ```
  qemu-system-x86_64 -cdrom sutureos.iso -m 512 -smp 2 \
      -device rtl8139,netdev=n0 -netdev user,id=n0
  ```
- 启动后试：`date`、`systest`、`pipetest`、`rtctest`、`mkdir /demo` + `cp` + `ls`

### ⚠️ 已知限制（这就是"ring 3 那半"）

- **仍然没有用户态**：`/init` 从不执行、`dynlink.c` 无人引用、没有 libc 落地。因此
  `dup`/`dup2`/`ftruncate`/`readlink`/`symlink` 返回 `-ENOSYS`（fd 表没有 dup API、
  VFS 没有符号链接），`wait4` 返回 `-ECHILD`（没有父子记账）。
- 信号投递的代码路径已接上，但没有 ring-3 程序可验证它真的进 handler；目前只能在
  内核里验证 pending 位置位/清位这一层。
- 未动：e1000（VMware 联网）、FAT32 LFN、ext2 写路径、GUI 拖动/缩放、`sudo -i` 之外的
  用户态工具。
