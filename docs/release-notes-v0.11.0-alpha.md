## 🚀 本次更新 (v0.10.1 → v0.11.0)

**用户态真的"完整"了。** ring 3 现在**中断开着**、每个程序跑在**自己的地址空间**
（真 CR3 切换）、`execve` 能替换当前程序，并且有一个**可交互的 ring-3 shell**。
这三件事是"完整用户态"的分水岭。

### ✨ 新增

- **每进程地址空间**（`kernel/syscall/userspace.c`）：每个程序一个真 PML4 —— 内核映射共享，
  1 GiB..2 GiB（内核恒等映射里空着的那一格）放私有 PDPT/PD/PT 给 `0x40000000` 窗口，
  于是两个进程可以用同一个 VA 却看到不同物理页。进 ring 3 切 CR3、退出切回内核 CR3。
  页表页从 PMM 取（静态 MMU 池跑几次就耗尽），退出时归还。
- **ring 3 开中断（IF=1）**：程序在 CPL=3 运行时硬件中断照常投递，系统节拍照走。
- **`execve`**（自有 ABI，`int 0x80` 号 9）：用另一个内建程序重建当前地址空间，并改写
  中断帧（rip/rsp/rax），`iretq` 直接落在新入口。
- **ring-3 交互 shell**（`user/apps/shell.c`，6889 B）：通过 `int 0x80` read(fd=0) 读命令
  （内核回显 + 退格），支持 `help/hello/probe/crash/pid/ticks/exec/exit`；内核 shell 用
  `ushell` 进入。
- **ring 3 的控制台输入**：`int 0x80` 的 read(fd=0) 接到键盘环并回显。
- **`userspace_selftest`**（9 项）进开机日志。

### 🛠 关键修复（每条都是这轮实测出来的）

| 现象 | 根因 | 修法 |
|---|---|---|
| CPL=3 #PF err=0x14 @ 0x40000000 无限循环 | `el0_enter_user_saved` 在**清零寄存器之后**才切 CR3，而清零块把 `%rdi`（PML4 参数）清了 → CR3 从未切换 | 把 `mov %rdi,%cr3` 移到清零块之前 |
| 窗口映射不生效 | `0x40000000` 是 1 GiB = **PML4 第 0 项**（不是第 1 项，第 1 项从 512 GiB 开始） | 私有 PDPT 装进 `pml4[0]` |
| `crash` 命令挂死 | CPL=3 的 #PF 被走 COW 路径 → `sig_send(current=-1, SIGSEGV)` → 重复执行故障指令 | 用户态缺页只杀该程序（exit 128+14），内核继续 |
| `execve` 后下一次 `syscall` #GP、SP 变垃圾 | `syscall` 触发的退出长跳出去、跳过配对的 `swapgs`，GS 被留成交换态 | 跨 ring 3 保存/恢复 GS MSR |
| 提示符刷屏 / 一次按键变一条命令 | EAGAIN 时重印提示符；`read(0)` 有一个字符就返回 | shell 累积到换行再执行 |

### ✅ 验证

- 回归全绿：`userspace selftest` **9/9**、`ring3test` **9/9**、`int80 selftest` **2/2**；
  其余不变（自检 13/14、rtc 8/8、pipe 16/16、FAT32 写+ops PASS、宿主 17/17、`AP 1 online`、
  RTL8139 ping、PS/2 鼠标、sudo 23/23）

- 交互 ring-3 shell 实测（串口原样）：

```
sutureos# ushell
SutureOS ring-3 shell (pid=1, argc=1)
commands: help hello probe crash pid ticks root exec exit
user$ ticks
ticks=1792736763
user$ hello
[user] execve('/hello') ok, entry=0x40000000
hello from Vimtu64 C userland
[HELLO] argc=1 argv0=user64 pid=1 ticks=1792736763
sutureos# ushell
user$ crash
writing to kernel memory at 0x100000 ...
[ring3] user #PF at 0x100000 (err=0x7) -> killed (exit 142)
[user] 'shell' exited with 142        <- 内核活着，回到内核 shell
sutureos# ushell                      <- 地址空间可反复创建
user$ exit
bye
```

### 📦 下载与体验

- 附件 `sutureos.iso`（13.10 MiB，13,731,840 字节）
- sha256：`3e2180cdc00ecfd6d52a8b579089ea684e891653520c5b72d893b3da747e2e4e`
- QEMU：
  ```
  qemu-system-x86_64 -cdrom sutureos.iso -m 512 -smp 2 \\
      -device rtl8139,netdev=n0 -netdev user,id=n0
  ```
- 进 shell 后输入 `ushell` 进入 ring-3 shell；试 `help`、`hello`（execve）、`crash`（隔离）
- 其它验收命令：`ring3test`（9/9）、`systest`、`pipetest`、`rtctest`、`sudotest`

### ⚠️ 已知限制（离"发行版级用户态"还差什么）

- **没有 `fork`/`wait4`**：一次只有一个进程（调度器仍是已知的双调度器状态），
  所以 shell 不能"跑一个程序再回来"，`execve` 是替换自己。
- 用户程序仍内嵌在内核镜像里（启动路径没用 `loader.c` 的 ELF 加载）。
- 系统调用执行期间仍关中断（CPL=3 的 ISR 帧已被定时器实测覆盖，嵌套重入未验证）。
- ring 3 只有 `shell`/`hello`/`probe` 三个程序；VimtuOS 的 `busybox/`、`shell/` 可沿同一
  流水线继续搬。
