## 🚀 本次更新 (v0.9.0 → v0.10.0)

把 **VimtuOS 的最小用户态 libc 整包移植**进 SutureOS，并配上跑它需要的全部内核机械：
固定 VA 的扁平 blob 加载器、VA→PA 用户页映射、Vimtu64 自有 ABI 的 `int 0x80` 门。
移植过程反查出 **三个内核真 bug** 并修好。

⚠️ **诚实说明**：移植的 libc 程序本身**还不能跑**——`int 0x80` 的寄存器帧布局不匹配
（下面写了原因和修法）。所以这一版交付的是**基础设施 + 内核修复**，不是"完整用户态"里程碑。

### ✨ 新增

- **`user/lib/`**：VimtuOS 最小 libc 近乎原样移植（同族项目，GPL-3.0）
  - `string.c`、`stdlib.c`（真 `malloc`/`free` 空闲链表竞技场）
  - `stdio.c`（带缓冲 `printf`/`puts`）、`syscall.c`/`syscall.S`（自有 ABI + Linux 号兜底）
  - `crt0.S`（入口 → `main(argc,argv,envp)` → `exit`）与一整套头文件
- **`user/lib/user.ld`**：扁平 blob 链接脚本 @ VA `0x40000000`
  （`.text.start` 第一、`-mcmodel=large` 的 `.l*` 段、`.image_end` 哨兵）
- **`user/apps/hello.c`** + **`user/apps/probe.c`**（分级验收：int80 write → puts →
  printf → malloc → getpid/ticks → exit(7)）
- **构建集成**：`build.py` 编译 libc 一次、逐程序链接成 blob 并嵌进内核
  （`_binary_<name>_bin_start/end/size`，32 KiB 装载窗口有断言）
- **`kernel/syscall/int80.c`**：向量 0x80（DPL=3）上的 Vimtu64 自有 ABI 门，
  转发 write/read/open/close/getpid/ticks/sleep_ms/exit，其余 `-ENOSYS`
- **`mmu_map_user_pages()` / `mmu_prepare_user_range()`**：固定用户 VA 窗口 → PMM 物理页
- **`user_run_blob()` / `user_run_builtin()` / shell `run`**：通用 ring-3 程序启动器

### 🛠 三个内核真 bug（都是这轮反查出来的）

| 现象 | 根因 | 修法 |
|---|---|---|
| 日志停在无害的 `zeroed page 7` 后整机静默 | **`IA32_EFER.NXE` 从未置位**，用户栈用 PTE bit63 标不可执行时该位是**保留位** → 每次碰栈都保留位 #PF | 开 EFER.NXE |
| `console_printf("%s", name)` 在函数内部 #PF，`CR2=0xffffffffffffffda`（= -38） | **`el0_resume_kernel()` 破坏 SysV ABI**：只恢复 rsp 就 ret，`rbx/rbp/r12-r15` 还是用户态残留 → 调用者局部变量变垃圾 | enter 时保存、resume 时弹回这 6 个寄存器 |
| 内核自己 exit 时越界写 | **`sys_exit()` 用 `tid == -1` 索引 `sched_cpu.tasks[tid]`**（ring-3 由 kmain 启动，current 是 -1） | 加范围校验 |

### 🧭 已知阻塞（为什么移植的 libc 还不能跑）

`int 0x80` **本身是通的** —— asm 探针程序通过它调 `getpid` 并拿到了返回值；但对 libc
程序，处理器读到的调用号是错的（日志 `nr=0` / `nr=-38`）。原因：

> `struct moos_regs` 声明 `rax` 在偏移 `+0x00`，而共用 ISR stub 是**先 push rax**，
> 所以它实际落在 `+0x70`。

**修法**：给 `int 0x80` 写专用 stub，用寄存器把 `(nr, a1, a2, a3)` 传给 C，而不是走这个帧。
在修好之前 `user_run_builtin()` 拒绝启动 libc 程序并打印原因（跑起来会挂机）。

### ✅ 验证

- 回归全绿：自检 13/14、FAT32 写+目录操作 PASS、宿主 17/17、`AP 1 online`、RTL8139 ping、
  PS/2 鼠标、rtc 8/8、pipe 16/16、sudo 23/23、`ring3test` 9/9；asm ring-3 程序仍然
  `hello from ring 3` / `int80 getpid=3` / `uname sysname=Linux` / exit 42
- `hello`（5161 B）与 `probe`（10249 B）都链接在 `0x40000000` 并嵌进内核；加载器映射、
  拷贝、`iretq` 进 CPL=3 都成功（QEMU `-d int` 能看到它发出的 `int 0x80` 调用），
  止步于上面那个帧 bug

### 📦 下载与体验

- 附件 `sutureos.iso`（13.06 MiB，13,690,880 字节）
- sha256：`c59fd9e53f8717d3bce05b7304a9b7a70976cc638c0249d91fd2478a90590d54`
- QEMU：
  ```
  qemu-system-x86_64 -cdrom sutureos.iso -m 512 -smp 2 \
      -device rtl8139,netdev=n0 -netdev user,id=n0
  ```
- 进 shell 后可 `ring3test`（9/9）、`run`（会说明 libc 程序暂不可运行）

### ⚠️ 已知限制

- 移植的 libc 程序不可运行（见上）；`user/apps` 目前只有 `hello`/`probe`
- 用户程序仍以 **IF=0** 运行（CPL=3 中断返回路径未修）
- 单一页表、无每进程地址空间 → 还不能 `fork`/`execve` 第二镜像
- `loader.c` 的 ELF 加载路径仍未用于启动路径
