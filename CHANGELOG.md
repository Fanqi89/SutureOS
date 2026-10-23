# Changelog

All notable changes to SutureOS are documented here.
The version numbers follow [Semantic Versioning 2.0.0](https://semver.org/) and
every release is cut as an annotated git tag `v<version>` whose message also
describes the release. Tags are never moved or deleted, so all versions coexist
in the repository.

## [v0.11.3-alpha]

补完第 4、5 项：socket 系统调用 + DHCP/DNS 客户端 + 真正的 signal 语义（sigframe +
rt_sigreturn）。启动呈现与 GUI 外观零变化。

### Added
- **socket 系统调用家族**（`kernel/syscall/sockets.c/.h`）：socket/connect/accept/sendto/
  recvfrom/sendmsg/recvmsg/shutdown/bind/listen/getsockname/getpeername/setsockopt/
  getsockopt 注册到内核既有 UDP/TCP 对象上，带用户指针校验；socket fd 进入 fd 表。
  `syscalls: 76 registered`（v0.11.2 为 62）。
- **DHCP 客户端**（`kernel/net/dhcp.c/.h`）：DISCOVER/REQUEST over UDP 68->67，解析
  OFFER/ACK 后写入 IP/掩码/网关/DNS。**只在 shell 命令 `dhcp` 时运行，不在开机阶段发起。**
- **DNS 解析器**（`kernel/net/dns.c/.h`）：A 查询构造 + 应答解析，`nslookup <name>` 触发。
- **signal 真语义**（`kernel/syscall/signal.c`）：进入 handler 前在用户栈上构造 sigframe
  （保存完整上下文 + 返回跳板），`rt_sigreturn` 从 sigframe 恢复全部寄存器与
  rip/rsp/rflags，handler 返回后精确回到被打断处；`sigaction`/`sigprocmask` 掩码真实
  生效；`SIG_DFL` 仍是终止该进程（不停机）。
- **ring-3 验收程序 `user/apps/sigtest.c`**：安装 SIGUSR1 handler -> `kill(getpid())`
  -> 校验 handler 运行并通过跳板返回 -> 成功退出 0；注册进内建程序表（`run sigtest`）。

### Verification
- 启动日志：`syscalls: 76 registered`、自检 13/14、`ring3 selftest 9/9`、
  `userspace selftest 9/9`、`[fat32] write self-test PASS`、无 PANIC；
  **启动日志行数 265 == 基线 265，零增删行**（只有数据值不同）。
- 新自检全部"成功不打印、失败才打印"。
- GUI 目录零改动，启动 print 序列未改动。

## [v0.11.2-alpha]

补完 5 项未完成系统调用中的前 3 项（dup/*at/ioctl+tty）。**启动呈现与 GUI 外观零变化。**

### Added
- **`dup`/`dup2`/`dup3`（32/33/292）**：fd 表新增"复制条目"能力；`dup` 取最小空闲 fd，
  `dup2` 指定目标 fd（已打开先关闭），管道端点复制会正确增加读写引用计数以保持
  EOF 语义；不再返回 `-ENOSYS`。
- **`creat`（85）**：等价 `open(path, O_CREAT|O_WRONLY|O_TRUNC)`。
- **`openat`/`mkdirat`/`unlinkat`/`renameat`/`fstatat`（257/258/263/264/262）**：
  `AT_FDCWD` 走当前 cwd，真实 dirfd 经 fd 表解析后拼接相对路径，再复用非 `*at` 版本
  的 VFS 实现。
- **`statx`（332）**：填充 `struct statx` 基础统计（mask=STATX_BASIC_STATS），
  时间戳按秒/纳秒给出。
- **最小 tty 层（`kernel/vfs/tty.c`/`.h`）**：fd 0/1/2 视为控制台 tty，
  `ioctl` 支持 `TCGETS/TCSETS/TCSETSW/TCSETSF`（termios 存取）、`TIOCGWINSZ`
  （返回控制台行列数）、`TCFLSH`；其它请求仍返回 `-ENOTTY`。原来所有 `ioctl`
  都是 `-ENOTTY`。

### Verification
- 启动日志：`syscalls: 62 registered`（v0.11.1 为 56，新增 6 个）。
- 回归不变：自检 13/14（`moos_intr` 既有判定）、`ring3 selftest 9/9`、
  `userspace selftest 9/9`、无 PANIC。
- 新增自检（**成功不打印**，失败才打印）：dup 双写同一文件、dup2 到固定 fd、
  `openat(AT_FDCWD,..)` 与非 at 版本一致、`statx` 返回 size>0、`ioctl(TCGETS)`/`TIOCGWINSZ` 成功。

### Not in this release
- **socket 家族（41-50）与 DHCP/DNS**：未做（下一批）
- **signal 真语义（sigframe + `rt_sigreturn`）**：未做（下一批）；当前 `SIG_DFL` 只杀进程、
  handler 仍只改 rip

## [v0.11.1-alpha]

补完三项"未完成/坏了"的部分，**启动呈现与 GUI 外观零变化**（见验证）。

### Fixed
- **用户态 ABI 号段冲突（真 bug）**：`user/lib/vimtu64.h` 把 9/10/11 声明为
  `FB_MAP`/`FB_FLIP`/`FB_PRESENT`、12/13/14 为 `INPUT_POLL`/`SHM_CREATE`/`SHM_MAP`，
  而内核 `int80.c:47` 把 **9 用作 execve**——用户一调 `fb_map()` 就会执行 execve。
  内核尚未实现这 6 个功能，现改为保留号段 `0x40..0x45` 并注明"调用返回 -ENOSYS"。
- **`SIG_DFL` 会停机整机**：`signal.c` 的默认动作原本调 `sched_shutdown()`，任何
  `SIG_DFL` 投递都会关掉整台机器。现在只终止该用户程序（退出码 128+sig），
  内核上下文的投递直接丢弃。

### Added
- **`mprotect`（系统调用 10）**：真实的 PTE 权限变更（W/NX 位）+ TLB 刷新；
  未映射页返回 `-ENOMEM`，非页对齐/零长返回 `-EINVAL`。
- **`munmap` 真实解除映射**：原实现是空壳（只 `return 0`）。现在清 PTE 并刷 TLB，
  unmap 后再访问会缺页而不是"静默还能用"（物理页暂不归还 PMM，注释已写明）。

### Verification
- 新增开机自检（成功时**不打印**，失败才打印，保持启动日志与基线一致）：
  `mmap -> mprotect -> munmap` 往返 + ABI 号段检查 = PASS。
- 回归不变：自检 13/14、`syscalls: 56 registered`、`ring3 selftest 9/9`、
  `userspace selftest 9/9`、FAT32 写+目录操作 PASS。
- **外观未变（按新规矩逐项验）**：
  - 终端截图与 `docs/baseline/base_terminal.png` **逐像素完全相同**（`bbox=None`）
  - GUI 截图与基线差异仅在 `(44,102)-(352,221)` = Task Manager 的实时计数器文字
    （运行时长/帧数），窗口、任务栏、配色、字体均一致
  - 启动日志逐行对比：仅内核地址（+0x1000）、PMM 空闲页数（-1）、RTC 时间不同，
    流程与文案完全一致

## [v0.11.0-alpha]

The userspace becomes real: interrupts stay enabled at CPL=3, every program runs in
its own address space (CR3 switch), `execve` replaces a running program, and there is
an interactive ring-3 shell.

### Added
- **Per-process address spaces** (`kernel/syscall/userspace.c`): a real PML4 per
  program - kernel map shared, the 1 GiB..2 GiB slot (free in the kernel map) gets a
  private PDPT/PD/PT for the user window at 0x40000000, so two processes can use the
  same VA and see different pages. CR3 is switched on entry to ring 3 and back on
  exit. Page-table pages come from the PMM (the static MMU pool ran out after a few
  runs), and are released on exit.
- **Ring 3 runs with IF=1**: hardware interrupts are delivered while a program is at
  CPL=3 (the timer keeps ticking), so userspace is preemptible in principle.
- **`execve`** as own-ABI `int 0x80` number 9: rebuilds the current address space
  with another built-in program and rewrites the interrupt frame (rip/rsp/rax) so
  `iretq` lands at the new entry point. Used by the shell's `hello`/`probe` commands.
- **Ring-3 interactive shell** (`user/apps/shell.c`, 6889 B): reads commands through
  `int 0x80` read(fd=0) (the kernel echoes them), and supports help/hello/probe/
  crash/pid/ticks/exec/exit. The kernel shell enters it with `ushell`.
- **Console input for ring 3**: `int 0x80` read on fd 0 is routed to the keyboard
  ring with echo and backspace handling.
- **`userspace_selftest`** (9 checks) wired into the boot log.

### Fixed
- **`el0_enter_user_saved` switched CR3 after zeroing the registers**, which cleared
  `%rdi` (the PML4 argument) - so CR3 was never switched and ring 3 ran on the kernel
  map where 0x40000000 is unmapped (QEMU: CPL=3 #PF err=0x14 at 0x40000000 forever).
- **The window is at 1 GiB, i.e. PML4 index 0** (not 1, which starts at 512 GiB), so
  the private PDPT belongs in `pml4[0]`.
- **#PF from CPL=3** was handled by the COW path, which ended in
  `sig_send(current=-1, SIGSEGV)` and re-executed the faulting instruction forever.
  A user-mode page fault now kills the program (exit 128+14) and the kernel survives:
  `[ring3] user #PF at 0x100000 (err=0x7) -> killed (exit 142)`.
- **GS MSRs are saved/restored across ring 3**: a `syscall`-initiated exit
  (`SYS_EXIT`) long-jumps out of the entry path and skipped the matching `swapgs`,
  leaving GS swapped for the next entry.
- Two shell input bugs: the prompt was reprinted on every `-EAGAIN`, and a single
  keystroke was treated as a command (read(0) returns as soon as one char is
  available, so the shell accumulates until newline).

### Verification
- `userspace selftest: RESULT PASS (9/9)`, `ring3 selftest 9/9`,
  `int80 selftest 2/2`, regression otherwise unchanged (self-tests 13/14, rtc 8/8,
  pipe 16/16, FAT32 write+ops PASS, host 17/17, AP online, ping, mouse, sudo 23/23).
- Interactive ring-3 shell transcript: `help`/`ticks`/`pid` work, `hello` execve's the
  libc program in the same address space, `crash` (write to kernel memory) is killed
  with exit 142 while the kernel survives, and `ushell` can be re-entered repeatedly.

### Known limitations
- No `fork`/`wait4`: one process at a time (the scheduler still has the known
  dual-scheduler problem), so the shell cannot run a program in the background and
  return to itself - `execve` replaces it.
- The userspace helpers are in the kernel image (no ELF loader on the boot path yet).
- Interrupts *during* a syscall stay disabled (the ISR stub frame handling at CPL=3
  is exercised by the timer, not by nested re-entry).

## [v0.10.1-alpha]

The ported userland libc runs. v0.10.0 shipped the port with one diagnosed blocker
(the `int 0x80` frame layout); this patch fixes it and enables the programs.

### Fixed
- **The `int 0x80` frame offsets.** `struct moos_regs` declares `rax` at +0x00, but
  the shared ISR stub pushes the GPRs as `rax, rcx, rdx, rbx, rsi, rdi, rbp,
  r8..r15`, so in memory the layout is `r15 ... rcx, rax` - i.e. rax is at **+0x70**
  (the struct's tail, vector +0x78 / error +0x80 / rip +0x88, is what really
  matches, which is why every existing handler worked). `kernel/syscall/int80.c`
  now reads the arguments and writes the result with explicit offsets
  (`FRAME_RAX/RDI/RSI/RDX`). Before this, the handler saw `nr=0` / `nr=-38` and the
  libc programs hung in ring 3 with no output.

### Verification: the ported libc runs
```
[user] running 'probe': va=0x40000000 len=10249 stack=0x4000fff0
[probe] 1 int80 write ok
[probe] 2 puts ok
[probe] 3 printf ok argc=1 argv0=user64
[probe] 4 malloc ok len=32 str=abcdefghijklmnopqrstuvwxyzabcdef
[probe] 5 pid=0 ticks=1792728129
[probe] 6 exiting with 7
[user] 'probe' exited with 7
[user] running 'hello': va=0x40000000 len=5161 stack=0x4000fff0
hello from Vimtu64 C userland
[HELLO] argc=1 argv0=user64 pid=0 ticks=1792728129
[user] 'hello' exited with 0
int80 selftest: RESULT PASS (2/2)   (14 calls served, 0 -ENOSYS)
```
That is: a C program built against the ported libc runs at CPL=3, its `printf`
reaches the console through `int 0x80` -> `write(2)`, `malloc` and `string.h`
work, and its exit code comes back to the kernel.

### Regression
self-tests 13/14, rtc 8/8, pipe 16/16, ring3test 9/9, FAT32 write+ops PASS, host
17/17, AP 1 online, RTL8139 ping, PS/2 mouse, sudo 23/23 - all unchanged.

## [v0.10.0-alpha]

Ported a real userland libc from VimtuOS and wired the machinery to run it, which
uncovered and fixed three genuine kernel bugs. The ported programs themselves are
**not runnable yet** - the `int 0x80` own-ABI gate reads the wrong call number
(diagnosed below) - so this release ships the infrastructure and the kernel fixes,
not the milestone.

### Added
- **`user/lib/`** - VimtuOS' minimal libc, ported nearly verbatim (GPL-3.0, same
  project family): `string.c`, `stdlib.c` (real `malloc`/`free` free-list arena),
  `stdio.c` (buffered `printf`/`puts`), `syscall.c`/`syscall.S` (own ABI +
  Linux-number fallbacks), `crt0.S` (entry, `main(argc,argv,envp)`, `exit`), and
  its headers (`stdio.h`, `stdlib.h`, `string.h`, `unistd.h`, `errno.h`,
  `unistd.h`, `vimtu64.h`, `sys/types.h`, `sys/stat.h`, ...).
- **`user/lib/user.ld`** - flat-blob linker script at VA `0x40000000` (VimtuOS used
  4 GiB); `.text.start` first, `.l*` sections for `-mcmodel=large`, `.image_end`
  sentinel so the `.bss` tail is part of the blob.
- **`user/apps/hello.c`** and **`user/apps/probe.c`** (staged acceptance program:
  int-0x80 write -> puts -> printf -> malloc -> getpid/ticks -> exit(7)).
- **Build integration**: `scripts/build.py` compiles the libc once, links each app
  with the flat-blob script and embeds the blob in the kernel image
  (`_binary_<name>_bin_start/end/size`), 32 KiB load-window limit enforced.
- **`kernel/syscall/int80.c`** - the Vimtu64 own-ABI gate on vector 0x80 (DPL=3):
  write/read/open/close/getpid/ticks/sleep_ms/exit forwarded onto the Linux-numbered
  syscall table; everything else answers -ENOSYS.
- **`mmu_map_user_pages()` / `mmu_prepare_user_range()`** - map a fixed user VA
  window onto PMM pages (U/S=1, W for data, NX for stack), which is what a program
  linked at an absolute address needs.
- **`user_run_blob()` / `user_run_builtin()` / shell `run`** - the general ring-3
  program launcher (the v0.9.0 runtime only knew the one embedded asm blob).

### Fixed (all three found by this work)
- **`IA32_EFER.NXE` was never set.** `mmu_map_user_pages()` marks the user stack
  non-executable with PTE bit 63; with NXE off that bit is *reserved*, so every
  stack access raised a reserved-bit #PF. Symptom: the machine stopped silently
  right after "zeroed page 7" (QEMU `-d int` showed `v=0e CR2=0x0`).
- **`el0_resume_kernel()` violated the C ABI** (SysV): it restored only `rsp` and
  returned, leaving `rbx`/`rbp`/`r12-r15` holding whatever the user program left
  in them, so the caller's locals were garbage (`console_printf("%s", name)` with
  name == -38 -> #PF inside `console_printf`, CR2=0xffffffffffffffda). It now
  saves those registers in `el0_enter_user_saved()` and pops them on the way back.
- **`sys_exit()` indexed `sched_cpu.tasks[tid]` with `tid == -1`** when the kernel
  itself is the caller (the ring-3 programs run from kmain), writing out of bounds
  and corrupting kernel state. Guarded like the fd syscalls.

### Known blocker (why the ported programs do not run yet)
`int 0x80` *works* - the asm probe program calls `getpid` through it and gets an
answer - but for the libc programs the handler reads the wrong call number
(`nr=0` / `nr=-38` in the log). The reason: `struct moos_regs` declares `rax` at
offset +0x00, while the shared ISR stub pushes `rax` first, so it lands at +0x70.
The proper fix is a dedicated `int 0x80` stub that passes `(nr, a1, a2, a3)` to C
in registers instead of through that frame. Until then `user_run_builtin()`
refuses to start the libc programs (running one hangs the machine) and says so.

### Verification
- Regression unchanged and green: self-tests 13/14, FAT32 write+ops PASS, host
  17/17, AP 1 online, RTL8139 ping, PS/2 mouse, rtc 8/8, pipe 16/16, sudo 23/23,
  `ring3test` 9/9, and the asm ring-3 program still prints
  `hello from ring 3` / `int80 getpid=3` / `uname sysname=Linux` and exits 42.
- `hello` (5161 B) and `probe` (10249 B) build and link at 0x40000000 and are
  embedded in the kernel; the loader maps them, copies them and enters ring 3
  (QEMU `-d int` shows the calls), but the own-ABI frame bug stops them there.

## [v0.9.0-alpha]

**Ring 3 runs.** The audit's biggest gap - "everything is in ring 0, there is no
userland" - is closed: a real CPL=3 program executes, makes syscalls through the
`syscall` instruction, gets kernel data copied into its own buffer, and returns
an exit code to the kernel.

### Added
- **`user/init.S`**: the first user program (334-byte flat image, no libc). It
  calls `write(1, ...)`, `uname()`, `getpid()` and `exit(42)`.
- **`kernel/syscall/ring3.c`**: the ring-3 runtime - PMM-owned user pages mapped
  with U/S=1, SSE enabled (CR4.OSFXSR|OSXMMEXCPT, CR0.EM/TS clear), GS base set up
  for the syscall entry, `el0_validate` armed, 16 KiB dedicated syscall stack.
- **`el0_enter_user_saved()` / `el0_resume_kernel()`**: enter CPL=3 and come back
  when the program exits (`sys_exit` recognises a ring-3 caller).
- **Fault isolation**: an exception taken at CPL=3 kills the user program with
  Linux's `128+signal` convention and the kernel keeps running instead of
  panicking (or, worse, re-executing the fault forever).
- **`ring3` / `ring3test`** shell commands + a boot-time run with a 9-check
  acceptance suite.

### Fixed (each of these blocked ring 3)
- **`el0_enter_user()` was broken three ways** and had never been called: it
  pushed/popped its two arguments in the wrong order (CR3 got the *context
  pointer*), every `user_context_t` offset was wrong (fs_base read from offset
  112 = rax), and it built an iret frame then executed `sysretq`. Rewritten for a
  single address space with `iretq`.
- **`syscall.S` built an incomplete frame** (only the 15 GPRs), so `ctx->cs` /
  `ctx->rip` were read from stale stack - which is why the signal-delivery hook
  never fired. It now saves a complete `user_context_t` and returns with `iretq`.
- **`IA32_EFER.SCE` was never set**, so the very first `syscall` raised #UD
  (vector 6 at user rip+0x1c). Also fixed `STAR`: the SYSCALL field held 0x10
  (kernel *data*) instead of 0x08 (kernel *code*), and the SYSRET field was wrong.
- **`fd_tables_init()` was never called**: the per-task fd table was all zeros,
  so `write(1)` did not take the stdout path and `open()` from ring 3 would always
  have failed with EMFILE.
- **`sched_cpu.current == -1`** while the kernel itself makes syscalls (the
  boot-time ring-3 run) indexed `fd_tables[-1]` out of bounds and swallowed the
  program's output. fd 1/2 are now resolved before the table lookup, and every
  fd syscall validates the task slot.
- **The ring-3 syscall ran on the launcher's stack**, overwriting the return
  address that `el0_enter_user_saved()` had saved - so `exit(60)` "returned" into
  garbage (observed: kernel #UD at rip=0x3). It has a dedicated stack now.

### Verification
- Boot log with the program running end to end:
  ```
  [ring3] entering CPL=3: entry=0x0000000000daa000 ...
  hello from ring 3
  uname sysname=Linux
  pid=0
  [ring3] the user program returned (exit=42)
  [  OK  ] ring3: first user program ran, exit=42
  ```
- `ring3test`: **9/9 PASS** (blob, user pages, SSE, FPU flags, GS base, program
  ran, exit code 42, user PTE U=1, kernel text U=0).
- Regression unchanged: self-tests 13/14, FAT32 write+ops PASS, host 17/17,
  AP 1 online, RTL8139 ping, PS/2 mouse, rtc 8/8, pipe 16/16, sudo 23/23.

### Known limitations
- The program runs with **IF=0**: a hardware interrupt taken at CPL=3 does not
  return correctly yet (the legacy same-CPL ISR stubs assume the kernel frame),
  so interrupts are re-enabled only after control is back in ring 0. The program
  needs no timer; a real ring-3 preemptive path is the next step.
- One shared page table with U/S bits rather than a per-process address space, so
  no `fork`/`execve` of a second image yet; the ELF loader (`loader.c`) is wired
  to the new entry but unused by the boot path.
- Still no libc in ring 3 (the program is hand-written assembly), and
  `dup`/`ftruncate`/`readlink`/`symlink` remain `-ENOSYS` as documented in
  v0.8.0-alpha.

## [v0.8.0-alpha]

The ring-0 half of the audit's "unfinished" list: the syscall surface a libc
needs, a real wall clock, pipes, file-management commands, and signal delivery
wired into the one return path this kernel has.

### Added
- **56 syscalls registered** (was 22). New: `stat`/`fstat`/`lstat`, `access`,
  `lseek`, `ioctl`, `dup`/`dup2`, `fsync`/`fdatasync`/`sync`, `truncate`,
  `statfs`, `chdir`/`getcwd`, `mkdir`/`rmdir`/`unlink`/`rename`, `getdents64`,
  `chmod`/`umask`, `getpid`/`gettid`/`getppid`, `getuid`/`geteuid`/`getgid`/
  `getegid`/`setuid`/`setgid`, `uname`, `gettimeofday`/`time`/`clock_gettime`
  (now the real clock)/`nanosleep`/`times`, `kill`/`wait4`/`exit_group`,
  `sysinfo`/`getrlimit`/`setrlimit`/`prctl`/`sched_getaffinity`/`getcpu`,
  `pipe`/`pipe2`. Relative paths resolve against the cwd, `stat` fills Linux's
  byte-exact 144-byte `struct stat`, and `uname` reports `sysname = "Linux"` with
  release `6.1.0-sutureos` so a libc's checks pass.
- **CMOS RTC driver** (`arch/x86_64/rtc.c`): the wall clock was faked (a constant
  1234567890); it now reads the RTC once at boot (BCD/binary, 12h/24h, century
  register) and anchors the tick counter on it. `date` prints real UTC.
- **Pipes** (`kernel/ipc/pipe.c`): pipe fds live in their own number space and
  `read`/`write`/`close` route to them; EOF when writers close, `-EPIPE` with no
  readers, short writes when full, bounded wait when empty.
- **Shell**: `rm`, `mkdir`, `rmdir`, `touch`, `cp`, `mv`, `date`, `uname`, `free`,
  and three acceptance commands `systest` / `pipetest` / `rtctest`.
- **Signal delivery**: `sig_check_pending()` is called on the syscall return path
  for user tasks (it had no call site at all), and `kill(2)` delivers into it.

### Fixed
- **ramfs `rename` was a stub** returning `VFS_EPERM`: implemented as a
  same-directory rename (what `mv a b` in one directory needs) with EEXIST/EXDEV
  checks, which is what made `systest`'s rename check fail at first.
- **RTC 12-hour decoding**: the PM flag (bit 7) was still present when the hour
  was converted, so "1 PM" became 141 instead of 13. Strip it before converting.
- RTC init decoded the year twice (BCD-decoding an already-binary year), which
  made every read look implausible and left `date` at 1970-01-01.
- `date` padding: this kernel's `snprintf` has no `0` flag, so the formatter now
  pads manually (`2026-10-22 23:04:11 UTC`, not `2026-10- 1  1: 4:11`).
- The self tests used `%-43s`, which `console_printf` prints literally (its
  formatter has no `-` flag) - replaced with plain `%s`.

### Verification
- `systest`: **39/39 PASS** (stat/lseek/read/write/fstat, mkdir/getdents64/
  unlink/rmdir, rename, chdir/getcwd, getpid/getuid/geteuid, uname, real
  clock_gettime/gettimeofday/time, pipe over the syscall path, sysinfo,
  getrlimit, sync, and the honest -ENOSYS/-ENOTTY answers).
- `pipetest`: **16/16 PASS**; `rtctest`: **8/8 PASS** (BCD/binary/12h/leap/format).
- Interactive: `date` -> `2026-10-22 23:06:17 UTC`; `mkdir /demo`, `touch`,
  `cp /etc/motd /demo/b.txt` (92 bytes), `ls` (a.txt, b.txt), `mv` (c.txt),
  `rm`, `rmdir`, `free`, `uname -a` all worked.
- Regression unchanged: self-tests 13/14, FAT32 write+ops PASS, host 17/17,
  AP 1 online, RTL8139 ping, PS/2 mouse, sudo 23/23.

### Still open (the ring-3 half, next batch)
- No userland yet: `/init` is never executed, `dynlink.c` is unreferenced, and
  `dup`/`dup2`/`ftruncate`/`readlink`/`symlink` answer `-ENOSYS` because the fd
  table has no dup API and the VFS has no symlinks. e1000 (VMware networking),
  FAT32 LFN, ext2 write and the GUI work are untouched.

## [v0.7.0-alpha]

First real privilege model: uids/gids, `/etc/passwd` + `/etc/shadow` with hashed
passwords, a `/etc/sudoers` policy parser, and a `sudo` that behaves like Linux's
sudo (ticket cache, 3 password tries, NOPASSWD, `-l/-k/-v/-u/-i`, audit log).

### Added
- **Crypto primitives** (`lib/sha256.c`, `include/stitch/sha256.h`): SHA-256
  (FIPS 180-4), HMAC-SHA256 (RFC 2104) and PBKDF2-HMAC-SHA256 (RFC 2898), all
  covered by known-answer self-tests that run at every boot - a broken hash would
  otherwise let every password through.
- **User database** (`kernel/security/userdb.c`): parses `/etc/passwd`,
  `/etc/group` (Linux formats) and `/etc/shadow`, seeding them on first boot with
  `root`/`root` (uid 0) and `user`/`user` (uid 1000, member of `wheel`). Password
  hashes are salted PBKDF2-HMAC-SHA256 stored as
  `$sutureos$pbkdf2-sha256$<iters>$<salt>$<hash>` - deliberately not Linux's
  crypt format, because it is not one.
- **Credentials** (`kernel/security/creds.c`): per-task uid/euid/suid/gid/egid
  (Linux shape). `cred_set_euid(0)` fails for a normal user on purpose; only
  `cred_escalate_root()` - called by sudo after authentication - may raise it.
- **sudo** (`kernel/security/sudo.c`, `sudoers.c`): Linux sudo's contract -
  `sudoers` syntax (`user`/`%group HOST=(RUNAS:GROUP) [NOPASSWD:] CMND`, `Defaults
  timestamp_timeout`/`passwd_tries`), timestamp ticket cache (15 min, `-k`
  invalidates, `-v` refreshes), 3 attempts with `Sorry, try again.` and
  `sudo: 3 incorrect password attempts`, both denial messages, `sudo -l`,
  `sudo -u`, `sudo -i`, and one audit line per invocation in `/var/log/sudo.log`.
- **Shell**: `su [user]`, `whoami`, `id`, `passwd [user]` (two hidden prompts),
  `sudotest` (23-check acceptance), a prompt that shows the identity
  (`sutureos# ` as root, `user@sutureos$ ` otherwise) and real permission checks
  on the privileged commands `mount`/`umount`/`shutdown`/`reboot`/`kill`.

### Fixed
- **ramfs files were capped at 64 bytes** (`uint32_t data[16]`): `/etc/motd` had
  silently been truncated and `/etc/passwd` could not exist at all. Nodes now
  carry `XJ_VFS_MAX_FILE_SIZE` (2048) bytes - this is what made the user database
  possible.
- **Dropped keystrokes at typing speed**: the 8042 has a one-byte output buffer
  and the shell read it once per idle iteration, *after* draining the SMP work
  queue and the NIC ring, so fast input was lost (typing `su user` arrived as
  `user`). A 64-byte input ring is filled with a burst of polls and drained by
  `console_getc()`; the keyboard is now serviced before the other idle work.
- PBKDF2 iterations lowered from 10000 to 2000: 10k cost ~1 s per verification,
  which stalled the shell (and made the password prompt feel broken).

### Verification
- `sudotest`: **23/23 PASS** (hashes, accounts, shadow round-trip, policy,
  ticket, credentials, audit log).
- Interactive QEMU transcript: `su user` -> `user@sutureos$`; `mount ...` denied
  with `permission denied (are you root? try 'sudo mount')`; `sudo -l` lists the
  policy after the password; `sudo mount fat32 0 /mnt/sudomnt` actually mounts and
  logs `RUN user=user runas=root cmd=mount ...`; a second `sudo` runs without a
  password (ticket); `sudo -k` forces the prompt again; 3 wrong passwords produce
  `sudo: 3 incorrect password attempts`; `su root` restores `sutureos#`.
- Regression: self-tests 13/14, FAT32 write+ops PASS, host verifier 17/17,
  AP online, RTL8139 ping, PS/2 mouse - unchanged.

### Known limitations (stated, not hidden)
- This is a **policy layer, not a security boundary**: every task still runs in
  ring 0 and the shell is the only user, so kernel code can bypass sudo. It
  becomes a real boundary when ring-3 userland lands; the cred API already has
  Linux's uid/euid/suid shape so the switch is mechanical.
- Linux sudo itself is a setuid userspace program built on PAM/crypt(3)/termios.
  None of those exist here (no libc in the image, 22 syscalls), so the behaviour
  was ported rather than the binary; password hashing is PBKDF2-HMAC-SHA256
  instead of yescrypt, and there is no secure RNG yet (salts mix the TSC, a
  counter and the user name).
- No `sudoedit`, no PAM modules, no LDAP, no `Defaults` beyond the two above, no
  per-command argument granularity beyond the command name.

## [v0.6.2-alpha]

Keyboard fix release. Typing lost characters (the shell's first keystroke after
boot, and whole words depending on mouse traffic) and the desktop froze a few
seconds after `startgui`, which also made it look like "the keyboard does not
work". Both root causes are documented below.

### Fixed
- **Keyboard starvation (characters/words swallowed)**: the 8042 output buffer
  is shared by keyboard and mouse, and `mbos_kbd_poll()` returned `-1` on an aux
  (mouse) byte *without reading it*. The buffer therefore stayed full and the
  keyboard byte behind it was never read, so typing died until an IRQ1 handler
  happened to consume the aux byte as a scancode. Reproduced on the released
  v0.6.1-alpha: typing `startgui` at the prompt produced no input at all.
  `mbos_kbd_poll()` now drains aux bytes into the mouse decoder
  (`mbos_mouse_feed_byte()`) and returns only scancodes.
- **GUI froze ~4 s after startgui** (frames stopped while `g_tick_count` kept
  advancing, so the desktop looked dead and ignored the keyboard): the frame
  loop called `sched_yield()`, which switches to a stale EEVDF context that
  never comes back. Removed; `gui_pace()` already blocks for one tick.
- **Mouse was initialised twice** (once at boot, again on GUI entry): the second
  `0x20` config-byte read returned a pending data byte (observed `0x9e`) and its
  `0xF6` went unanswered. GUI entry now only queries `mbos_mouse_present()`.
- **`scripts/build.py` / `build_iso.py` hardcoded ROOT** to the main checkout, so
  building from a git worktree silently compiled the wrong sources into the main
  `build/` dir and left stale objects that got linked into a later build (which
  surfaced as an intermittent "hangs after `[SMP] starting AP 1`"). ROOT now
  follows the script location, with `SUTUREOS_ROOT` as an override.

### Added
- `[kbd] key #n sc=.. ch=..` diagnostics (first 12 keys) and a `[gui] frame=..`
  heartbeat every 1000 frames: they separate "the byte never arrived" from "it
  decoded to 0" and "the loop died" from "the timer died" in serial-only runs.

## [v0.6.1-alpha]

Fixup release: the About window was still printing a hardcoded `v0.5.0-alpha`,
so a stale version string was baked into the ISO. Found by the new release
verification, which now also proves the version string is inside the artifact.

### Fixed
- GUI "About SutureOS" reads the version from `include/sutureos/version.h`
  instead of a literal, so the ISO can no longer ship an old version string.

### Added
- `scripts/verify_release.ps1`: proves a release is real - version string inside
  the ISO (and the previous version gone), asset `size`/`digest` read back from
  the API by asset NAME, optional byte-range download-back, every OLD release
  compared against a pre-publish snapshot, and the git side in sync.
- `scripts/publish_all.ps1`: the whole release in one command (snapshot ->
  commit/tag/push -> GitHub Release + ISO -> verification).
- `docs/RELEASING.md` documents the sequence and the checks.

## [v0.6.0-alpha] - unreleased

Kernel hardening on real hypervisors, a much more complete network stack, the
SquashFS filesystem, the boot-presentation rework and a usable GUI pointer.

### Fixed
- **AHCI/SATA on VMware**: the driver treated a lingering `DRQ` bit at command
  completion as a failure (`tf not idle after completion`), which made every
  read fail and left FAT32/ext2/SquashFS unmountable. Completion is now judged
  by `ERR`/`DF` + the fatal `PxIS` bits only, with a bounded settle wait and a
  sticky "device quirk" flag so the wait is paid once.
- **ACPI with a lot of RAM (hang right after `[SMP] BSP LAPIC timer armed`)**:
  with 4 GiB the firmware places the RSDT/MADT above the kernel's identity map
  and the first dereference froze the boot. ACPI structures are now mapped on
  demand (`acpi_touch()`), every length is validated before walking, and the
  MADT subtable walk stops on a zero-length entry instead of spinning.
- Terminal clear actually clears: flanterm does not implement `ESC[2J`, so the
  clear was a no-op; it now erases row by row (`ESC[<row>;1H` + `ESC[2K`), flushes
  the pending double buffer first, and wipes the framebuffer pixels that the
  early fbcon log left behind.

### Added
- **TCP passive open** (`tcp_listen`/`tcp_listen_accept`) plus a one-slot
  out-of-order reassembly buffer, verified by three independent acceptance
  paths (guest as client 7/7 + 6/6, guest as server through QEMU `hostfwd` 8/8,
  and an in-kernel injected-segment self-test `tcpooo`).
- **Socket ABI for `SOCK_STREAM`**: `socket/bind/listen/accept/connect/send/recv/
  shutdown/close` now route to the TCP stack; `tcpapi` echoes through that ABI
  only (8/8).
- **SquashFS 4.0 read-only driver** (gzip data blocks, compressed and raw
  metadata, fragments, id table, fast/extended symlinks), with a shared RFC1951
  inflate extracted from the deb code. Verified by a self-written image
  generator, an independent verifier (83/83) and a host harness that compiles
  the real driver (138/138).
- **Boot presentation**: Limine boots straight into the kernel (no menu), the
  English log scrolls, then the screen is cleared and a SutureOS banner is
  printed before the shell prompt; `startgui` enters the desktop.
- **Console log ring** (128 lines) and `log` / `dmesg` commands that auto-scroll
  to the newest line.
- **PS/2 mouse driver** (8042 aux port, polled), an on-screen arrow pointer, the
  taskbar system name turned into a **start button whose menu pops upwards**
  (Task Manager / Calculator / About SutureOS / Exit to terminal), clickable
  title-bar buttons and client-area clicks forwarded to apps.
- `mouseinfo` diagnostics (8042 status, config byte, device id, packet counts)
  and `startgui test` (pointer self-test through the real packet decoder).
- Host-side harnesses: `vmware_boot.ps1`, `vmware_ui_probe.ps1`,
  `tcp_server_acceptance.py` and a single-sourced version header
  (`include/sutureos/version.h`, also parsed by the ISO manifest).

### Changed
- FAT32 ops self-test now also covers `mkdir`/`rmdir`, `rename` and `unlink`
  (host verifier extended to 17 checks, including "no orphaned cluster chains").

## [v0.5.0-alpha]

Snapshot that the repository started from: GUI framework with Task Manager /
Calculator / About, TCP/IP + UDP/ICMP, several filesystems, SMP bring-up and the
Limine-based ISO. (Tagged retroactively on the commit whose message reads
"完成GUI、TCP/IP、多文件系统等重大更新，升版至 v0.5.0-alpha".)

## [v0.1.0-alpha]

Initial commit: bootable 64-bit kernel with 12/12 self-tests passing.
