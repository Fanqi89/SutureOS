## 🚀 本次更新 (v0.8.0 → v0.9.0)

**Ring 3 跑起来了。** 审计里最大的缺口——「所有东西都在 ring 0，没有用户态」——补上了。
一个真正的 CPL=3 程序在跑：它用 `syscall` 指令进内核、让内核把 `uname` 数据拷进自己的
缓冲区、最后把退出码还给内核。内核侧有 U/S 页权限隔离和用户态异常隔离。

### ✨ 新增

- **`user/init.S`**：第一个用户程序（334 字节扁平镜像，**无 libc**），依次调用
  `write(1, ...)`、`uname()`、`getpid()`、`exit(42)`。
- **`kernel/syscall/ring3.c`**：ring-3 运行时 —— 从 PMM 取的物理页按 U/S=1 映射给用户、
  开 SSE、设置 syscall 入口要用的 GS base、武装 `el0_validate`、给 syscall 配 16 KiB 专用内核栈。
- **`el0_enter_user_saved()` / `el0_resume_kernel()`**：进 CPL=3，程序 `exit` 时回到内核。
- **用户态异常隔离**：CPL=3 下的异常按 Linux 的 `128+signal` 杀掉该程序，内核继续跑，
  既不 PANIC 也不重复执行同一条故障指令。
- **`ring3` / `ring3test`** shell 命令，开机自动跑一次并输出 9 项验收。

### 🛠 关键修复（每一条都曾把 ring 3 挡死）

| 现象 | 根因 | 修法 |
|---|---|---|
| 进不去用户态 | `el0_enter_user()` 从未被调用，且三处写错：参数 push/pop 反序（CR3 被装成上下文指针）、`user_context_t` 每个偏移都错、构造 iret 帧却执行 `sysretq` | 重写为单地址空间 + `iretq` |
| 信号投递钩子从不触发 | `syscall.S` 只保存 15 个通用寄存器，`ctx->cs`/`ctx->rip` 是栈上残留值 | 保存完整 `user_context_t`，`iretq` 返回 |
| 第一条 syscall 就 #UD | `IA32_EFER.SCE` 从未置位；`STAR` 的 SYSCALL 字段写成 0x10（内核数据段）而非 0x08（内核代码段） | 置 SCE、修正 STAR |
| `write(1)` 不打印、ring 3 的 `open()` 必然 EMFILE | `fd_tables_init()` 从未被调用，fd 表全是 0 | 开机初始化 fd 表 |
| 用户输出被吞 | 内核自己发 syscall 时 `sched_cpu.current == -1`，`fd_tables[-1]` 越界读到 `vfs_fd=0` | fd 1/2 在查表前解析 + 校验任务槽 |
| `exit(60)` 返回后内核 #UD `rip=0x3` | ring-3 的 syscall 跑在启动者栈上，覆盖了 `el0_enter_user_saved()` 存的返回地址 | 专用 16 KiB 内核栈 |

### ✅ 本次验证

开机日志（完整链路，串口原样）：

```
[ring3] entering CPL=3: entry=0x0000000000daa000 stack=0x0000000000db1fc0 blob=334 bytes
hello from ring 3
uname sysname=Linux
pid=0
[ring3] the user program returned (exit=42)
[  OK  ] ring3: first user program ran, exit=42
```

`ring3test`：**9/9 PASS**（镜像内嵌、用户页已映射、SSE 已开、FPU 标志已清、GS base 可达、
程序跑到 exit、退出码 42 传回、用户 PTE U=1、内核 text U=0）

回归不变：自检 13/14、FAT32 写+目录操作 PASS、宿主 17/17、`AP 1 online`、RTL8139 ping、
PS/2 鼠标、rtc 8/8、pipe 16/16、sudo 23/23

### 📦 下载与体验

- 附件 `sutureos.iso`（13.00 MiB，13,629,440 字节）
- sha256：`aedd0ceff84c8d4673d1be60c29c531c1375b661b7c816f7099869403cb768b1`
- QEMU：
  ```
  qemu-system-x86_64 -cdrom sutureos.iso -m 512 -smp 2 \
      -device rtl8139,netdev=n0 -netdev user,id=n0
  ```
- 开机时自动跑用户程序（日志里能看到 `hello from ring 3`）；进 shell 后可用 `ring3`、`ring3test`

### ⚠️ 已知限制（明说）

- **用户程序以 IF=0 运行**：CPL=3 下被硬件中断打断后，现有的同特权级 ISR 栈帧假设会导致
  返回错乱，所以中断只在回到 ring 0 后才重新打开。程序不需要定时器；「ring 3 抢占式中断
  返回」是下一步。
- **单一页表 + U/S 位**，还不是每进程独立地址空间，所以还不能 `fork`/`execve` 第二个镜像；
  `loader.c` 的 ELF 加载路径已接到新入口但启动路径没用到。
- ring 3 仍然**没有 libc**（程序是手写汇编）；`dup`/`ftruncate`/`readlink`/`symlink` 仍按
  v0.8.0-alpha 的说明返回 `-ENOSYS`。
