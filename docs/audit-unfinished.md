# SutureOS 未完成 / 半成品 / 桩代码审计

- **审计对象**：`main` @ `fa43ffa`（v0.11.5-alpha），**只读静态审计**（未构建、未改源码）
- **证据格式**：`文件:行号`；行号一律用 `git show HEAD:<file>` 取，不受工作树改动影响
- **快照说明**：审计开始（17:28）工作树是干净的；审计过程中出现并行未提交改动（新增
  `kernel/drivers/e1000.c/.h`，修改 `kernel/fs/fatfs.c`、`kernel/main/kmain.c`、
  `kernel/net/net.c`、`kernel/net/net.h`），另有 detached worktree `build/wip-tree` @
  `d6405f4`（`wip/all-ports` 分支）。**本报告不考虑这些未提交内容**，只在"可移植项"里点名。
- 复核方法见文末第 7 节。

---

## 1. 概要

**一句话结论**：内核侧（内存/块设备/VFS/网络/TCP/ring-3/多进程）确实"能跑且有宿主侧验收"，
但一个可日常使用的系统所必需的四件事全部缺位 —— **没有持久化（ramfs 根 + 无安装器）、
没有系统级权限边界、没有能跑真实程序的用户态（execve 只认 5 个内嵌镜像）、没有中文与
USB**；同时 README/CHANGELOG/docs 里积压了 21 处"已经做完却仍写着未做"的过期条目，
把真实缺口埋在文档噪音里。

| 等级 | 条数 | 含义 |
|------|------|------|
| **P0** | **7** | 阻塞性 / 已声称支持但实际不可用或有内核破坏风险 |
| **P1** | **19** | 明确缺失的重要能力（有明确的移植方向） |
| **P2** | **30** | 打磨项：死代码、过期注释、文档与日志不实 |

另外两条对决策有用的事实：

- 系统调用真实注册数是 **102**，启动日志写的是 **81**（只统计了 `syscalls2.c` 的 81 个），
  而 `linux_compat.c` 映射了 **276** 个 Linux 号 → **179 个映射号会返回 `-ENOSYS`**（P0-4）。
- `wip/all-ports` 已经有 **e1000 驱动** 和 **FAT32 VFAT 长文件名（读侧）** 两个移植成果
  （4 个提交，只基于 v0.11.0），main 上没有 —— 这是当前性价比最高的两项"待合入"。

---

## 2. P0（阻塞性 / 声称支持但不可用）

### P0-1 重启即丢失一切：根文件系统是内存 ramfs，且没有系统安装器

- **现象**：`/` 挂的是 `xj_vfs`（内存文件系统，静态节点池），`/etc/passwd`、`/etc/shadow`、
  `/etc/sudoers`、用户自己 `touch` 的文件全部只存在于内存；磁盘（FAT32/ext2/SquashFS）
  只被挂到 `/mnt/*` 且 ext2/SquashFS 只读。ISO 只能 live 启动，没有"安装到硬盘"的路径。
- **证据**：
  - `kernel/main/kmain.c:296-297` — `/* Mount the in-memory ramfs as root filesystem */` + `vfs_mount("ram", "/", "xj_vfs")`
  - `kernel/main/kmain.c:302-315` — 每次开机重新种 `/hello.txt`、`/etc/motd`
  - `kernel/main/kmain.c:387-411` — 磁盘只 auto-mount 到 `/mnt/fat`；ext2→`/mnt/ext2`、SquashFS→`/mnt/squash`（`:420-462`）
  - 无系统安装器：`git grep -iE 'install to disk|system installer'` = 0 命中；
    `installer` 的 2 处命中只是 `.deb` 包安装器（`kernel/pkg/deb.c:1`）与 Limine 注释（装 .deb ≠ 装系统）
- **影响**：任何"用一下、重启、东西还在"的承诺都不成立；改密码、装包、写文件一次重启全没了。
  这是当前离"能日常用"最远的一项，也是所有其它努力都会被它清零的一项。
- **建议修法**：二选一（可并行）：
  1. 最小可用：把根迁到一个可写 FAT32/ext2 卷（先加 ext2 写路径，或让 ramfs 定期/卸载时
     把整棵树 sync 到一个 FAT32 卷），启动时优先从该卷加载 `/etc`；
  2. 做真正的安装器（分区表 + 文件系统格式化 + 把 ISO 载荷拷到盘 + 写引导项），
     main 上没有这部分，需要新写。

### P0-2 权限模型只存在于 shell 外壳：VFS/系统调用层零检查，ring-3 进程永远是 root

- **现象**：README 写"真正的权限模型，不再是模拟""权限真正生效"（`README.md:374`、`:396-397`），
  但实测：`open()` 不查 uid/mode；`stat` 的 mode 是写死的 0755/0644；`chmod` 直接 `return 0`
  什么也不做；`chown` 是 no-op；`kill` 没有权限判断；任何 CPL=3 进程的
  `cred_current()` 都落在 slot 0（= 开机 root 任务）→ **用户程序一律 euid=0**。
- **证据**：
  - `kernel/vfs/vfs.c:180-196` — `vfs_open()` 只有 mount 查找/fd 分配，没有 cred 检查
  - `kernel/vfs/vfs.c:784` / `:788` — `st->mode = 0755;` / `st->mode = 0644;`（不来自磁盘）
  - `kernel/syscall/syscalls2.c:1024-1028` — `sys2_chmod()` 直接 `return 0; /* mode bits are reported, not enforced */`
  - `kernel/vfs/vfs64.c:108-119`、`:407` — chmod/chown/时间戳均为"documented no-op"
  - `kernel/security/creds.c:32-38` — `cred_current()`：`if (slot < 0 || slot >= SCHED_MAX_TASKS) slot = 0;`
    （`sched_cpu.current` 只有 `kernel/sched/task.c`/`eevdf.c` 会写，进程槽是
    `PROC_SLOT_BASE=8` 起的 `process.c` 私有表 → ring-3 进程取到的永远是内核 slot 0 = root）
  - `kernel/syscall/syscalls2.c:856-880` — `sys2_kill()` 无 uid 比较
  - `README.md:398-399` 自己承认"这是策略层，不是安全边界"，但 `:374`/`:396` 的措辞与之矛盾
- **影响**：`cat /etc/shadow` 对任何身份都成功；sudo 可以被任何程序绕过（进程本身就是 root）；
  文件权限位永远显示 0644/0755，用户误以为有隔离。等于**没有权限系统**。
- **建议修法**：① 让 ramfs/FAT32/ext2 节点持久化 `mode/uid/gid`；② 在 `vfs_open`/`vfs_stat`/
  `vfs_unlink` 等入口做"euid==0 或 owner/组/other 位"判定并返回 `VFS_EACCES`；
  ③ `sys2_chmod` 真写 mode；④ `cred_current()` 接 `proc_current_slot()`（`kernel/syscall/syscalls.c:37-45`
  已有这个正解），fork 时克隆 cred；⑤ `kill` 加 uid 检查。

### P0-3 `brk`/`mmap` 对 ring-3 进程不可用，且有越界写／内核页表污染风险

- **现象**：两个最基础的内存系统调用用的是**内核任务槽**的下标和**全局静态页池**，而 ring-3
  进程既不在 `sched_cpu.current` 里、也不拥有那个页池。`user_brk[tid]` 在 `tid==-1` 时越界写；
  `brk` 的映射循环从 `old`（初值 0）开始，会把 VA 0 起的低地址按 U=1 映射（用户可读写内核恒等映射区）；
  `mmap` 只能从 256 页静态池分配、永远不归还、且不置 NX。
- **证据**：
  - `kernel/syscall/mem.c:12` — `static uint64_t user_brk[SCHED_MAX_TASKS];`
  - `kernel/syscall/mem.c:18-19` — `int tid = sched_cpu.current;` / `uint64_t old = user_brk[tid];`
    （`sched_cpu.current` 只在 `kernel/sched/task.c`/`eevdf.c` 里赋值；`process.c` 不碰它）
  - `kernel/syscall/mem.c:25-28` — `for (v = old; v < new_brk; v += 4096) { … *pte = loader_alloc_page() | PTE_P|PTE_W|PTE_U; }`
  - `kernel/syscall/mem.c:52-57` — mmap 循环，`pte_val` 只有 `PTE_P|PTE_U`（无 NX）
  - `kernel/syscall/loader.c:71-81` — `loader_alloc_page()` 固定 256 页静态池、永不复用
  - `arch/x86_64/mmu.c:35` — `mmu_page_pool[8192]`（= 16 页）是 `mmu_walk` 唯一的页表来源，
    mmap 想把 1 GiB 窗口映射出来必然耗尽
  - `kernel/syscall/mem.c:108-114` — 还留着一个 `sys_munmap_legacy_unused()`（死函数，只 `return 0`）
- **影响**：in-tree 的 libc 自带 `.bss` 堆（`user/lib/stdlib.c:38`）所以现在没炸；但**任何真实
  libc/程序**（musl 的 malloc 大块走 mmap、启动时调 brk）一调用就可能越界写内核数据或污染低地址
  页表，症状会比"内存不够"严重得多。
- **建议修法**：按进程实现——`brk`/`mmap` 记到 `userspace_t`（而不是内核任务槽），页从 PMM 取、
  记入 COW/引用计数表（`kernel/syscall/userspace.c` 已有 frame ref 表可直接复用），
  映射区必须落在该进程私有窗口内并置 NX；`munmap` 归还页。

### P0-4 179 个已映射的 Linux 系统调用仍然返回 `-ENOSYS`（含 poll/select/readv/writev/pread/pwrite）

- **现象**：`linux_compat.c` 有一张 276 项的 Linux→本内核号映射表，但"本内核号是否注册"另行
  校验；未注册的直接 `-ENOSYS`。真实注册 102 个（启动日志报的 81 只是其中一批）。
  最常用的一批缺失：`poll(7)`、`select(23)`、`readv(19)`、`writev(20)`、`pread64(17)`、
  `pwrite64(18)`、`fchdir(81)`、`socketpair(53)`、`getrusage(98)`、`sendfile(40)`、
  `clock_nanosleep(230)`、`prlimit64(302)`、`memfd_create(319)`、`epoll_*`、`inotify_*`、
  `eventfd/timerfd`、`sem*/shm*/msg*`、`ptrace`、`mount/umount2/reboot`。
- **证据**：
  - `kernel/syscall/linux_compat.c:21-298` — 276 项映射表
  - `kernel/syscall/linux_compat.c:341-362` — 未注册 → `console_printf("[linux_compat] unimplemented Linux syscall %lu\n")` + `return -ENOSYS`
  - `kernel/syscall/syscalls2.c:1117-1252` — 81 次 `reg(...)`
  - `kernel/syscall/syscall.c:157-206` — 23 次 `syscall_register(...)`（2 处重叠 → 102）
  - 复核命令见 §7（对 SYS_ 定义、注册集合、映射集合做集合差 = 179）
- **影响**：目前只有 5 个自带程序能跑，正是因为它们只用到了那 102 个。任何从 Linux 移植的
  静态程序，第一轮初始化（locale/stdio/malloc 的 poll/readv 路径）就会撞 `-ENOSYS`，
  而且每个未实现号都会往串口打一行日志（刷屏 + 掉速）。
- **建议修法**：按"被调用频率"补齐一批即可解锁大量程序：
  `poll/select`（内核已有非阻塞 recv 与 3 s 忙等基础）→ `readv/writev/pread64/pwrite64`（fd 层加
  scatter/gather）→ `fchdir`、`socketpair`、`prlimit64`、`clock_nanosleep`、`getrusage`。
  另建议把 `linux_compat` 的"映射表"改成"注册表 = 唯一事实来源"，避免再次出现表很全、实则 179 项假象。

### P0-5 `futex`/`madvise`/`fcntl` 是"恒返回成功"的假实现

- **现象**：三个系统调用都是 `return 0`，注释直接写"简化实现：总是返回成功/返回 0"。
  `fcntl` 不实现 `F_GETFL/F_SETFL`（→ 无法设 O_NONBLOCK、无法查 fd 状态）；`futex` 不睡眠不唤醒
  （→ pthread 互斥/条件变量"成功"但语义全错）；`madvise` 不释放页（→ MADV_DONTNEED 说谎）。
- **证据**：`kernel/syscall/syscalls.c:650-655`（futex）、`:659-664`（madvise）、`:668-673`（fcntl）
- **影响**：任何依赖这些语义的程序会得到"成功但行为错"的结果，比 `-ENOSYS` 更难排查；
  `kernel/syscall/syscalls.c:590-601` 的 `getrandom` 同样是固定种子的 LCG（见 P1-14）。
- **建议修法**：`fcntl` 至少实现 `F_GETFL/F_SETFL/F_DUPFD/F_GETFD/F_SETFD` 且真的写 fd 表标志位；
  `futex` 要么按 `-ENOSYS` 如实拒绝，要么接进程阻塞队列（`process.c` 已有 BLOCKED 机制）；
  `madvise` 对未实现的 advice 返回 `-EINVAL` 而不是假装成功。

### P0-6 `execve` 只能执行 5 个内嵌镜像；ELF 加载器不可用于启动且会直接写"裸地址"

- **现象**：ring-3 的 `execve` 走到一个按名字查表的函数，表里只有 `hello/probe/shell/sigtest/forktest`
  五个内嵌 blob，其它路径 `-ENOENT`；真正的 ELF 路径 `sys_execve` 是另一份实现，把 PT_LOAD
  直接 `memcpy` 到 `p_vaddr`（当前地址空间！），并把 `rsp` 设成未映射的 `0x7FFFFFFFF000`、
  把 ELF 的 `min_vaddr` 当 PML4 传给 `task_set_user_pml4`。`loader.c` 里还有第三份
  `syscall_execve()`，零调用点，且第 110 行栈指针取到 `user_stack_pool[..][1024]`（越界一位）。
- **证据**：
  - `kernel/syscall/ring3.c:343-354` — `g_programs[]` 五元组
  - `kernel/syscall/ring3.c:360-389`、`:394-430` — `user_exec_in_place()` / `user_exec_from_frame()` 只按名字查表，否则 `return -2`
  - `kernel/syscall/syscall.c:38-47` — 只有 `ring3_active()` 时拦截 execve；否则落到 `sys_execve`
  - `kernel/syscall/syscalls.c:511-568` — `sys_execve`：`:551` 调 `elf_load_user`，`:558-559` rsp/rip，`:564-565` `task_set_user_pml4(tid, base)`
  - `kernel/syscall/loader.c:37-69` — `elf_load_user`：`:61-64` `uint8_t *dst = (uint8_t *)(uintptr_t)ph->p_vaddr; memcpy(dst, src, ph->p_filesz);`（无边界/无映射）
  - `kernel/syscall/loader.c:85-123` — 死代码 `syscall_execve()`；`:110` `&user_stack_pool[stack_pool_used][1024]`（越界）
  - `README.md:104` 仍把"ELF 加载器 / EL0 切换"标成 ✅
- **影响**：OS 无法运行任何外部程序（磁盘上的 ELF/.deb 里的可执行文件都不行），
  "用户态"始终停在 5 个演示程序；上面那两处 memcpy/pmap 一旦被走到就是内核数据损坏或 panic。
- **建议修法**：把 `userspace_create()` 建成"先建地址空间 → 按 p_vaddr 映射 → 再拷段"的顺序
  （现在 `loader` 是反的），删掉两条重复 execve 路径只留一条，并把 `g_programs[]` 换成
  "VFS 查文件 + ELF 校验"。

### P0-7 没有 PID 1 / 没有 init / 没有服务管理；`/init` 是假的

- **现象**：开机由 `kmain` 直接进内核 shell（或 `startgui`），没有任何用户态 init 进程；
  `user/` 里没有 init 程序（`user/init.S` 只是 kmain 启动时跑一次的 CPL=3 冒烟程序，不是 PID 1）；
  `vfs_read_file("/init")` 在文件不存在时会退回一个内嵌测试 ELF —— 看起来"支持 /init"，实际是假象。
- **证据**：
  - `kernel/vfs/vfs.c:620-628` — `if (strcmp(path, "/init") == 0) { … if (vfs_stat("/init",&st)!=VFS_OK) return test_program_binary(len); }`
  - `kernel/syscall/test_prog.c:1-55` — 手写字节的 1 个 ELF（只打印一行然后 exit）
  - `kernel/main/kmain.c:104`（`kernel_main`）→ `:348` `ring3_run_init()`（跑一次 init.S）→ `:364-366` 跑 `probe`/`hello`
  - `kernel/syscall/process.h:36` — "Create the session's root process (pid 1)" 是按需创建（`ushell`），不是开机
  - 全树 grep `/sbin/init|init process` 无实现，shell 55 条命令里也没有"服务/自启"概念
- **影响**：所有程序只能人肉从 shell 敲；没有"开机就有的东西"（网络配置、getty、守护进程）。
  这是"OS 架构"层面的缺口，不是某个驱动没写。
- **建议修法**：做一个最小 PID 1（用户态或内核线程皆可）：挂载 → 读 `/etc/init.d` 或
  `/etc/rc` → 拉起服务 → `wait4` 回收；同时把 `vfs.c` 里那个假 `/init` 删掉。

---

## 3. P1（明确缺失的重要能力）

> 每条给"证据"和"移植线索"（只写方向）。

**P1-1 USB 完全没有（HID 键鼠 / U 盘 / 控制器）**
证据：全树 `git grep -E 'usb_|USB|xhci|ehci|uhci'` = 0 命中；`kernel/drivers/` 无 USB 文件；
     输入只有 PS/2（`kernel/drivers/mb_kbd.c`、`mb_mouse.c`）。
影响：真机上绝大多数键鼠 U 盘走 USB；BIOS 传统仿真能救键盘，U 盘/平板/无 PS/2 的机器直接不可用。
线索：xHCI/EHCI 主机控制器 + HID 启动协议 + USB Mass Storage（BOT/SCSI）。
      可参考 ToaruOS、SerenityOS、U-Boot 的 USB 栈，或 Linux `drivers/usb/` 的初始化顺序。

**P1-2 main 上没有 e1000（VMware 默认网卡 → VMware 下无网）**
证据：HEAD 全树 `e1000` = 0 命中；`kernel/net/net.c:29` 只 `#include "rtl8139.h"`；
      `README.md:493`（已知问题 22）自述此事。
影响：VMware（用户主力环境之一）ping/DHCP/DNS/TCP 全部不可用。
线索：**已经写好了**——`wip/all-ports` 的 `290d7a7`（移植 Cinux e1000 + 链路层分发）、
      `4a89a89`（e1000 转首选网卡 + 自检修复）；直接 cherry-pick 这两提交，勿 merge 整条分支
      （该分支基于 v0.11.0，缺 v0.11.1~v0.11.5 的 fork/socket/signal/GUI 全部工作）。

**P1-3 FAT32 只有 8.3 短名，且并发打开文件上限 8**
证据：`kernel/fs/fatfs.c:10`（"8.3 names only, LFN entries are skipped"）、`:31` `MAX_FILES 8`、
      `:346`/`:976`/`:1117` 跳过 LFN 项。
影响：`hello.txt` 可以，`README.md`、中文名、长路径全部不行；写超过 8 个文件同时打开就失败。
线索：`wip/all-ports` 的 `d6405f4` 已实现"VFAT 长文件名**读侧**"+夹具/校验器，可直接摘；
      写侧（创建 LFN）还需另做。

**P1-4 根 ramfs 容量极小：64 个节点、单文件 2048 B、名字 64 字符**
证据：`kernel/vfs/vfs.h:196-201`（`XJ_VFS_MAX_NAME 64`、`XJ_VFS_MAX_NODES 64`、
      `XJ_VFS_MAX_FILE_SIZE 2048`）；`kernel/vfs/vfs.c:502` 静态节点池。
影响：`/etc`+`/usr` 种完就快满；写一个 3 KB 文件失败；装 deb 包稍微大点就 ENOSPC。
线索：把 ramfs 改成从 PMM/heap 动态分配 inode 与数据块（或直接用 tmpfs 思路）。

**P1-5 没有 ATAPI / 光驱支持**
证据：`kernel/drivers/block.c:476`、`:518` 显式跳过 ATAPI 签名；`kernel/drivers/ahci.c:238-239`、
      `:962-963` 同样拒绝 `AHCI_SIG_ATAPI`。
影响：光驱里的文件读不了（只能把 ISO 当启动介质）；真机/老机器装系统场景缺失。
线索：ATAPI packet command（READ(10)）套在现有 ATA PIO / AHCI 传输上，复用 `block.h` 接口即可。

**P1-6 没有声卡 / 音频子系统**
证据：全树 `AC97|HDA|audio|sound` = 0 命中（`kernel/drivers/` 无相关文件）。
影响：无声音、无提示音；多媒体无从谈起。
线索：Intel HDA 控制器 + codec 枚举（先从 QEMU `-device intel-hda` 验），或 AC'97 更简单。

**P1-7 显卡只有 framebuffer，没有 2D/3D 加速**
证据：`kernel/drivers/fbcon.c` 直接写线性帧缓冲；HEAD 全树没有任何 virtio-gpu 实现
      （唯一 2 处命中是 `kernel/drivers/virtio_blk.c:12`/`virtio_blk.h:11` 的出处注释路径）；
      GUI 全部是 CPU 画像素。
影响：1920×1080 下刷一帧要几毫秒，无硬件光标/无叠层；QEMU/VMware 下能忍，真机很慢。
线索：virtio-gpu 2D（QEMU 首选，最省事）→ 再考虑 Intel i915 的 modeset 子集。

**P1-8 中文（以及任何非 ASCII）完全不支持**
证据：`kernel/drivers/font.c:1-2`（"8x16 bitmap font for ASCII 32-126, 95 glyphs"）；
      `kernel/term/flanterm/flanterm.c:1152-1160` 只做 CJK **宽度**判定、没有字形；
      GUI 无 IME/无文本输入控件（全树 `clipboard|IME` = 0）；`user/lib/stdio.c` 无 UTF-8 处理。
影响：终端和 GUI 里中文一律是空白/替换符；中文项目最刺眼的一项。
线索：Unifont / Spleen / GNU Unifont 的 8x16/16x16 子集 + UTF-8 解码 + 双宽字符渲染
      （flanterm 的 `wcwidth` 已经就绪，只缺 glyph 数据）；输入法先做"剪贴板粘贴/拼音表"最小版。

**P1-9 GUI 能力很薄：3 个应用、无剪贴板、无文本输入、无 DPI 缩放**
证据：`kernel/gui/apps.c:242-261` 只注册 Task Manager/Calculator/About；
      `kernel/gui/gui.h:35-40` 里 `APP_ID_MINES`、`APP_ID_TERM` 定义了但没有任何实现；
      `kernel/gui/gui.h:16` `GUI_MAX_WINDOWS 16`；无剪贴板、无输入法、无 DPI 缩放
      （`git grep -iE 'clipboard|\bIME\b|zoom'` = 0；`scroll` 的 80 处命中全是终端滚屏/
      ScrollLock 键码，没有 GUI 滚动条）；`kernel/gui/gui.c` 无滚轮/双击处理。
影响：GUI 只能"看 + 按计算器"；没有终端窗口、文件管理器、文本编辑器，桌面不构成生产力环境。
线索：先补"文本输入控件 + 剪贴板 + 滚轮"，再把内核 shell 包成一个 GUI 终端窗口（APP_ID_TERM 已预留）。

**P1-10 调度/进程模型：无抢占、最多 8 进程、AP 不参与进程调度、三套调度器并存**
证据：`kernel/syscall/process.h:11` `PROC_MAX 8`；协作式切换（CHANGELOG v0.11.4 "Limits"，
      `CHANGELOG.md:68-71`）；`kernel/sched/task.c` + `kernel/sched/eevdf.c` +
      `kernel/syscall/process.c` 三套并存（`README.md:470` 已知问题 2 仍成立）；
      AP 只跑工作队列不进任务/进程调度（`README.md:258`）；`kernel/sched/prio_sched.c` 第四套死代码（P2）。
影响：一个自旋进程饿死所有兄弟；没有后台任务；多核只用于 IPI/工作队列。
线索：把 `process.c` 的协作切换接上 LAPIC 定时器抢占点（`arch/x86_64/lapic.c` 已有 100 Hz tick），
      再把 `task.c` 与 `eevdf.c` 合并成一套（删 `prio_sched.c`）。

**P1-11 网络：IPv6/IP 分片/路由表全无；TCP 是停等子集；DHCP/DNS 只能手工触发**
证据：`kernel/net/net.c:575`（"no reassembly in this build"）；IPv6 只有
      `kernel/net/net.h:28` 一个以太类型常量、没有任何协议实现；
      `kernel/net/net.c:87` 静态 `10.0.2.15`；`kernel/net/tcp.h:37` `TCP_MAX_CONNS 4`；
      TCP 无拥塞控制/SACK/窗口缩放/时间戳/零窗口/MSS 协商（`kernel/net/tcp.h:22-24` 自述，
      但其中 listen/accept 与乱序重组已实现，见 P2）；DHCP 只在 shell 命令里跑
      （`CHANGELOG.md:84-85`，`kmain.c` 无 dhcp 调用）；DNS 只查 A 记录（`kernel/net/dns.c`）；
      无 TLS/HTTP 客户端。
影响：只能访问 IPv4 局域网/slirp；HTTPS 时代等于"没有互联网"；开机不自动拿 IP。
线索：先"开机自动 DHCP + 路由表 + IP 分片重组"，再考虑 TLS 用 mbedTLS/BearSSL 裁剪版。

**P1-12 文件系统：ext2/SquashFS 只读；无 ext4/journal；无页缓存；无 /proc /dev /tmp**
证据：`kernel/fs/ext2.c:10-11`（"READ-ONLY: there is no write path"）、`kernel/fs/ext2_vfs.c:7-8`；
      全树无 ext4 实现（唯一命中是 `kernel/fs/ext2.h:16` 的"不支持 extents/journal"注释）；
      无 devfs/procfs/sysfs/tmpfs（grep 0）；无页缓存（`vfs_read_file` 整文件读进 heap，
      `kernel/vfs/vfs.c:620+`）。
影响：装不了系统到 ext2/4；`/proc` 缺失让任何 Linux 程序的自省逻辑失效；每次读文件都走块层。
线索：ext2 写路径（借鉴 ext2 规范即可）→ ext4 extents/journal（可参考 CoolPotOS/XJ380 的片段）；
      `/proc` 先做 `/proc/self/{exe,stat,fd}` 与 `/proc/meminfo`。

**P1-13 内存：没有 swap、没有页回收，`munmap` 不归还物理页，无文件支持的 mmap**
证据：`kernel/syscall/mem.c:90-106`（unmap 只清 PTE，注释明说"frames are not handed back to the PMM yet"）；
      swapon/swapoff 只存在于映射表未注册（`kernel/syscall/linux_compat.c:188-189`）；
      无 LRU/reclaim 代码；mmap 只接受 `MAP_ANONYMOUS|MAP_PRIVATE`（`kernel/syscall/mem.c:47`）。
影响：长时间运行必然内存耗尽；不能跑大于物理内存的程序/文件映射。
线索：先做"munmap/exit 归还 PMM + 每进程记账"，swap 可最后做。

**P1-14 安全加固为零：无 ASLR/KASLR/SMEP/SMAP/canary/CSPRNG**
证据：全树 `ASLR|canary|SMEP|SMAP` = 0 命中；用户态固定加载在 `0x40000000`
      （`kernel/syscall/userspace.h:25`）；`getrandom` 是固定种子的 LCG
      （`kernel/syscall/syscalls.c:590-601`，`userdb.c:175-176` 自己注释"getrandom() 是 LCG"）；
      `mmap` 不置 NX（`kernel/syscall/mem.c:55`），NX 只用于用户栈（`arch/x86_64/mmu.c:164-165`）；
      PTE_NX 位存在但 `EFER.NXE` 只在 `kernel/syscall/ring3.c:67-74` 打开。
影响：可执行堆 + 固定地址 + 可预测随机 = 任何内存破坏都是可利用的；密码盐可预测。
线索：`getrandom` 用 RTC/PIT 抖动 + TSC 混合作种子（`arch/x86_64/rtc.c` 已有真时钟）；
      开 SMEP/SMAP（CR4）+ 随机化用户窗口基址 + 栈 canary（编译器 `-fstack-protector` 已有工具链支持）。

**P1-15 线程不存在（clone(CLONE_VM) 直接 -EINVAL）**
证据：`kernel/syscall/syscalls2.c:905-910`（`if (flags & CLONE_VM_) return -EINVAL`）；
      `sys_futex` 是假实现（P0-5）。
影响：任何多线程程序无法移植；pthread 生态归零。
线索：clone 线程 = 共享 `userspace_t` 的第二个执行上下文 + futex 真实现（这是 Linux 兼容的最大单点）。

**P1-16 包管理与软件安装：只有开机时装一个写死路径的 hello.deb**
证据：`kernel/main/kmain.c:464-477`（`deb_install("/mnt/fat/hello.deb", "/", 1)` 硬编码）；
      `deb_remove`（`kernel/pkg/deb.c:873`）、`deb_query`（`:904`）、`deb_list_installed`（`:911`）、
      `deb_check_dependencies`（`:644`）**全部实现但零调用点**；无 shell 命令；
      xz/zstd 成员不支持（`kernel/pkg/deb.c:19-20`、`:612-613`、`:832-833`）；
      无维护者脚本（`kernel/pkg/deb.c:917-921` 如实返回 -1）；无网络下载。
影响：README 说"deb 安装器（真实现）…安装/卸载/查询/DB"（`README.md:107`），实际用户无法
      安装/卸载/查询任何包，只能看开机日志。
线索：加 `dpkg`/`apt` 风格的 3 条 shell 命令（install/remove/list）即可让已有 API 可用；
      xz 可接 `kernel/pkg/inflate.c` 旁边的 LZMA 解码器（SquashFS 已拒绝 xz，需新增）。

**P1-17 电源与硬件：只有 S5 关机 + reset；无 S3/S4、无电池/温度/CPU 频率、无热插拔/MSI**
证据：`arch/x86_64/power.c:1-14`（只有 S5 + RESET_REG→0xCF9→8042→triple fault）；
      无 sleep/hibernate 代码；AHCI 纯轮询（`README.md:276`）；NVMe 单页 bounce
      （`kernel/drivers/nvme.c:32-35`）；virtio-blk 静态 16 KiB DMA 环（`README.md:278`）。
影响：笔记本合盖/休眠不可用；真机看不到温度/电量；没有热插拔与中断（MSI）。
线索：ACPI S3（FADT SLP_TYP + 恢复路径）、EC/电池（ACPI EC 读）、`_TMP` 温度、PCI MSI 能力表。

**P1-18 UEFI 路径从未实测**
证据：`README.md:141`（"Limine (BIOS 已实测启动；UEFI 路径未实测)"）；仓库只提供 ISO，
      无 OVMF 自动化脚本（`scripts/` 无 uefi 相关脚本）。
影响：现代真机默认 UEFI-only，可能直接启动不了；这也是"支持 UEFI"这类说法的唯一证伪点。
线索：QEMU + OVMF（`-bios OVMF.fd`）跑一次现有 ISO，把串口判据写进脚本即可（工作量极小）。

**P1-19 用户态工具链：只有 5 个程序，且 shell 不支持管道/重定向/环境变量/通配符/作业控制**
证据：`user/apps/` 只有 `hello.c`、`probe.c`、`shell.c`、`sigtest.c`、`forktest.c`（+ `user/init.S`）；
      `user/lib/` 无 `opendir/readdir/getdents` 封装、无 `mkdir/rmdir/rename`、无
      `snprintf/scanf`、无 `fopen/fread/fwrite`、无 `getenv/setenv`、无 `qsort`；
      内核 shell 55 条命令（`kernel/shell/shell.c:57-111`）无 `|`、`>`、`*`、`&`、
      无 `export`、无编辑器。
影响：没有 `ls/cat/grep/cp` 这类可组合工具，没有编辑文件的可能；"用户态"无法自举。
线索：把 `busybox`/`toybox` 的静态子集沿 `user/lib` 流水线移植（README 已提到这条路）；
      shell 先加管道与重定向（`proc` 层已有 fd 复制）。

---

## 4. P2（打磨项）

1. `kernel/net/net.c:24` 注释写 "NOT implemented: TCP"，实际 TCP 已在 `:28` include 并工作。
2. `kernel/net/net.c:1411` 注释还叫 "socket API (stub ABI, datagram subset)"，v0.11.3 起已接
   SOCK_STREAM/TCP（`kernel/net/net.c` 的 `net_send`/`net_recv` 分支）。
3. `kernel/net/net.h:5` "the stub socket layer is kept for ABI stability" 同样过期。
4. `kernel/net/tcp.h:22-24` 的"honest list"里 `listen()/accept() backlog` 与
   `out-of-order reassembly` 已经实现（`kernel/net/tcp.c:264`、`:387`），只剩 backlog 队列语义没做。
5. `kernel/main/kmain.c:479-485` 大段注释写"FAT32 WRITE path is not trusted yet"，与紧随其后的
   已验证写自检（`:489-520`）直接矛盾，是 2026-10-13 误判的残留。
6. `kernel/main/kmain.c:333-334` 启动日志 "22 base" 与真实 23 个基础注册不符；且打印的
   "81 registered" 只数了 `syscalls2.c` 的 81 个，真实值 102（低报 21 个）。
7. `kernel/main/init.c:2`、`:32` "Using legacy PIC for timer (IOAPIC/LAPIC deferred)" 过期，
   节拍实际来自每核 LAPIC（`README.md:477`）。
8. `kernel/gui/apps.c:201` "there is no mouse driver in this build" 过期（鼠标已可用）。
9. `kernel/gui/gui.c:958-959` "keyboard-only build ... not clickable" 过期（标题栏按钮可点）。
10. `kernel/drivers/mb_mouse.c:250-259` 包长固定 3 字节、`:363-365` 只在诊断里识别 wheel ——
    从未发 `0xF3` 采样率序列启用滚轮；若设备以 4 字节包上报，整条流会错帧。
11. `kernel/syscall/mem.c:108-114` `sys_munmap_legacy_unused()` 死函数（只 `return 0`）。
12. `kernel/syscall/loader.c:85-123` 死代码 `syscall_execve()`；`:110` `user_stack_pool[..][1024]` 越界一位。
13. `kernel/ipc/fifo.c` 整个 `helos_fifo32_*` 零调用点（死模块）。
14. `kernel/sched/prio_sched.c` 零调用点（调度器已三套，这是第四套）。
15. `kernel/vfs/vfs64.c`（VimtuFS2 API，约 26 KB）里没有任何 `vfs64_*` 函数被外部调用
    （`git grep 'vfs64_[a-z0-9_]*(' HEAD -- ':!kernel/vfs/vfs64.*'` = 0；`squashfs.c:2019-2024`
    只借用了 `VFS64_*` 常量填结构，不调它的函数），等于死代码；README:100 已标 ❌，但它仍编进内核。
16. `kernel/syscall/dynlink.c`（22 KB，ELF 符号解析+TLS 重定位）零外部引用，但 `README.md:106` 仍标 ✅。
17. `kernel/syscall/test_prog.c` + `kernel/vfs/vfs.c:620-628` 构成"假 /init"（见 P0-7）。
18. `README.md:137-138` 两段重复，且都说 socket 号未注册（v0.11.3 已注册）。
19. `README.md:91` 说键盘"轮询式，无 IRQ 环形缓冲"，与 `:146` 的"IRQ1 已接通 + 64 字节环形缓冲"矛盾。
20. `README.md:649-651` "当前版本 0.1.0 / 最后更新 2025-10"，实际 0.11.5-alpha / 2026-11。
21. `README.md:5` "53 源文件 / 341 KB" 与实际不符（HEAD 跟踪 197 个源文件 ≈1.87 MB，
    `build/kernel.elf` 当前约 1.7 MB）；`:142` "~11.5 MB" 与 `:551` "10.8 MB" 互相矛盾，
    当前 `build/sutureos.iso` = 14,100,480 B（13.4 MiB）。
22. `docs/verification-report.md:1-13`（2024 / v0.1.0 / 12/12）与现状（v0.11.5 / 13/14）不符，应归档为历史。
23. `docs/build-and-test.md` 全篇过期：路径写"缝合怪系统"（`:43`）、`-kernel` 直通启动（`:64`）、
    判据 `[stitch] boot chain verified`（`:66`）、VMware"待做"（`:81-86`）。
24. `docs/architecture.md:107` "Phase-1（当前）" + Phase-2/3 路线图 —— 现在已经 v0.11.5（ring 3/多进程）。
25. `docs/provenance.md:115` 仍写旧路径 `C:\Users\fanqi\Desktop\缝合怪系统\`。
26. `scripts/build2.ps1`、`scripts/build3.ps1`、`scripts/build.bat` 零引用（死脚本）；
    `scripts/run-qemu.ps1` 的判据同样过期（且 `docs/build-and-test.md:69` 称它是唯一机械判据）。
27. `arch/x86_64/acpi.h:43` 与 `arch/x86_64/madt.h:12` 重复定义 `acpi_madt`/`acpi_madt_t`
    （`README.md:494` 已记录，仍未合并，任何同时包含两者的 TU 编不过）。
28. 两套名字上限：`kernel/vfs/vfs.h:10` `VFS_MAX_NAME 128` vs `:196` `XJ_VFS_MAX_NAME 64`。
29. 文件时间戳不落盘（`kernel/vfs/vfs64.c:407`），`stat` 的 atime/mtime/ctime 恒 0。
30. 内核 shell 无管道/重定向/环境变量/通配符/作业控制；Ctrl+C 无信号语义（无作业控制代码）。

---

## 5. 测试与验收缺口

**已有宿主侧机械化判据**（好）：FAT32 写/目录操作（`scripts/verify_fat32_write.py`）、
ext2 读（`verify_ext2_read.py`）、SquashFS 读（`verify_squashfs_read.py`）、ARP/ICMP/UDP
（`net_probe.py`）、TCP 主动/被动（`tcp_acceptance.py`/`tcp_server_acceptance.py`）、
发布自证（`verify_release.ps1`）、符号冲突（`tests/symbol-scan.ps1`）、
网卡校验和与 SquashFS 宿主 harness（`tests/net_harness`、`tests/squashfs_harness`）。

**只有内核内自检、无宿主侧独立判据**（自证风险）：PMM/heap/slab/queue/rbtree/vfs/sched/eevdf/intr、
RTC(8)、pipe(16)、sudo(23)、systest(39)、ring3(9)、userspace(9)、int80(2)、fork(`proc_selftest`)、
FAT32/ext2/SquashFS 的 mount 自检、AHCI/NVMe/VirtIO（全部搭 FAT32 写自检的便车）、socket ABI。

**完全没有自动化验收**（高风险）：
| 功能 | 现状 | 缺什么 |
|------|------|--------|
| GUI（拖动/缩放/关闭/开始菜单） | 只有 `scripts/vmware_ui_probe.ps1` 手工脚本 | 未纳入任何回归链；无像素级基线比对（`docs/baseline/` 有基线图但无脚本消费） |
| deb 安装器 | 只看开机串口日志 | 无宿主侧校验包内容是否真的落到 VFS；卸载/查询 API 零测试 |
| DHCP / DNS | 只能在 shell 手工敲 `dhcp`/`nslookup` | 无自动化；租约不持久化也未测 |
| 信号投递（sigframe/rt_sigreturn） | `run sigtest` 手工 | 无宿主判据；SIG_DFL/SIG_IGN/掩码组合未覆盖 |
| execve / ELF 加载 | 无 | 任意路径、坏 ELF、段越界均无测试 |
| 权限模型 | `sudotest` 内核内自检 | 无"普通用户读 /etc/shadow 应被拒"这类负例（因为功能本身不存在，P0-2） |
| 中文/UTF-8 | 无 | 无渲染基线 |
| 电源（S5/reset） | 有 QEMU monitor 判据（README 记录） | 未脚本化进回归 |
| 构建完整性 | `symbol-scan.ps1` | 无 CI（仓库无 `.github/`）、无统一回归入口、无静态分析 |

**建议**：新增 `scripts/regression.ps1` = 构建 → QEMU 启动解析串口断言（自检全 PASS、
`syscalls: 102 registered`、无 PANIC）→ 四个块控制器 verify → net/tcp 脚本 → GUI 截图与
`docs/baseline/` 比对；再把 `verify_release.ps1` 的 6 项自证并进来。这是把上面所有
"内自检"变成"可证伪验收"的最省力路径。

---

## 6. 已过期 / 应删的文档条目

> 结论：**21 处**。历史 CHANGELOG 条目建议加"（已由 vX.Y.Z 完成）"而不是删除；
> 其它文档建议直接改或归档到 `docs/history/`。

| 位置 | 原文 | 现在的事实 |
|------|------|-----------|
| `README.md:5` | "~53 源文件 / 341 KB 内核镜像" | HEAD 跟踪 197 个源文件（90 .c/96 .h/9 .S ≈1.87 MB）；kernel.elf 当前 ≈1.7 MB |
| `README.md:91` | "PS2 键盘（轮询式，无 IRQ 环形缓冲）" | `:146` 自己写 IRQ1 + 64 字节环形缓冲；两者矛盾 |
| `README.md:103` | 只列 read/write/open/close/brk/mmap/… 22 个 | 实际注册 102 个（stat/lseek/pipe/getpid/chdir/mkdir/getdents64/socket 家族…） |
| `README.md:137-138` | 两段重复："socket 号仍未在 syscall 表注册" | v0.11.3 已注册（`kernel/syscall/syscalls2.c:1219-1249`），`sockets.c` 有用户指针校验 |
| `README.md:142` / `:551` | "ISO 大小 ~11.5 MB" / "10.8 MB" | 当前 `build/sutureos.iso` = 14,100,480 B（13.4 MiB）；两处数字互相矛盾 |
| `README.md:442` | banner 示例 "version 0.5.0-alpha" | 真实版本 0.11.5-alpha（`include/sutureos/version.h`） |
| `README.md:470` | 已知问题 2："两套调度器并存，任务内 sched_yield 会跳到 0x3" | `sched_yield` 已分流（`kernel/syscall/syscalls.c:491-508`）；现在是三套（task/eevdf/process） |
| `README.md:484` | "残留限制：… TCP 未实现。" | TCP 已实现并有三条验收（`README.md:120-135`） |
| `README.md:487` | 已知问题 14："没有监听队列（listen/accept 仍返回 ENOSYS）、没有乱序重组" | v0.6.0 起 listen/accept 已实现、v0.11.3 起有 1 槽乱序缓冲（`kernel/net/tcp.c:264`,`:387`） |
| `README.md:649-651` | "当前版本 0.1.0 / 最后更新 2025-10" | 0.11.5-alpha / 2026-11 |
| `CHANGELOG.md:127-131` | v0.11.2 "Not in this release"：socket 家族与 DHCP/DNS 未做；signal 真语义未做 | 全部在 v0.11.3 完成（`CHANGELOG.md:74-99`） |
| `CHANGELOG.md:213-219` | v0.11.0 "Known limitations"："No fork/wait4：一次一进程" | v0.11.4 完成 fork(COW)+wait4+getppid（`CHANGELOG.md:34-73`） |
| `CHANGELOG.md:306-313` | v0.10.0 "Known blocker (why the ported programs do not run yet)" | v0.10.1 已修复（`CHANGELOG.md:226-234`），标题会误导 |
| `CHANGELOG.md:576` | "## [v0.6.0-alpha] - unreleased" | 早已发布并打了附注标签 |
| `docs/verification-report.md:1-13` | "Phase-1 / 2024 / v0.1.0 / 12/12" | 应归档为历史（当前 13/14 selftest、v0.11.5） |
| `docs/build-and-test.md:43` | `cd C:\Users\fanqi\Desktop\缝合怪系统` | 仓库目录是 `SutureOS`（旧名） |
| `docs/build-and-test.md:60-70` | `-kernel` 直通启动 + 判据 `[stitch] boot chain verified` | 现在走 ISO/Limine 启动，判据是自检 + `sutureos#` 提示符 |
| `docs/build-and-test.md:81-86` | "VMware 需先做可启动磁盘（Phase-2 任务）" | `scripts/vmware_boot.ps1` 已实现并用 VMware 17.5.2 实测 |
| `docs/architecture.md:107-110` | "Phase-1（当前）" + Phase-2/3 路线图 | 已到 v0.11.5（ring 3、每进程地址空间、fork/COW） |
| `docs/provenance.md:115` | 旧路径 `C:\Users\fanqi\Desktop\缝合怪系统\` | 应为 `SutureOS`（或直接用相对路径） |
| `scripts/run-qemu.ps1`（被 `docs/build-and-test.md:69` 当作唯一机械判据） | 等 `[stitch] boot chain verified` | 该字符串在 HEAD 已不存在（`git grep = 0`），脚本永远超时失败 |

---

## 7. 可移植性最高、收益最大的前 10 项（供决策）

按 **收益 / 工作量** 排序；"已有代码"指仓库里已经写好但没合入/没接线的部分。

| # | 事项 | 收益 | 工作量 | 依据 / 已有代码 |
|---|------|------|--------|------------------|
| 1 | **文档对账**：改 README/CHANGELOG 21 处、归档 3 份过期文档、把 4 处过期注释改掉、启动日志改成真实 102 | 极高（后面所有人的判断都基于文档；本次审计一半时间花在这里） | ~0.5 天 | 本报告 §6；注释在 `net.c:24`、`tcp.h:22`、`kmain.c:479`、`init.c:2` |
| 2 | **cherry-pick e1000**（`wip/all-ports` 的 `290d7a7`+`4a89a89`） | 高：VMware 有网，DHCP/DNS/TCP 全部活过来 | ~0.5 天 | `wip/all-ports:kernel/drivers/e1000.c`（354 行）+ 链路层分发 + v0.13 实测记录 |
| 3 | **cherry-pick FAT32 VFAT LFN 读侧**（`d6405f4`） | 高：长文件名（含中文名）可读，用户第一眼可见 | ~0.5 天 | `wip/all-ports` 的 `kernel/fs/fatfs.c` + 夹具/校验器 |
| 4 | **`fcntl`/`futex`/`madvise` 语义化 + `getrandom` 真熵源** | 高：解锁 musl 程序与 pthread 的前置；消除"假成功" | ~1 天 | `syscalls.c:650-673`；熵源用 `rtc.c`/PIT/TSC 抖动 |
| 5 | **补 poll/select + readv/writev/pread64/pwrite64/fchdir/socketpair/prlimit64** | 高：移植程序第一轮初始化就不再撞 ENOSYS | ~1.5 天 | fd 层已有 pipe/socket/vfs 三态；`net_dgram_recv` 已有超时参数 |
| 6 | **权限接进 VFS**（ramfs 存 mode/uid/gid + open/stat 检查 + chmod 真实现 + kill 校验 + cred 接 `proc_current_slot`） | 高：把"权限模型"从外壳变成真的；消除 `/etc/shadow` 可读 | ~1.5 天 | `vfs.c:180`、`syscalls2.c:1024`、`creds.c:32`、`syscalls.c:37` |
| 7 | **brk/mmap 按进程重做**（去掉 `sched_cpu.current` 依赖、页从 PMM 取、置 NX、munmap 归还） | 高：唯一的高危内存破坏点；真实 libc 的前置 | ~1.5 天 | `mem.c:12-60`；可复用 `userspace.c` 的 frame-ref/COW 表 |
| 8 | **中文：加 CJK 位图字体 + UTF-8 解码 + GUI 文本输入** | 高（对中文用户是"能不能用"的分界） | ~1.5 天（字体数据是大头） | `font.c:1`（95 字形）、`flanterm.c:1152` 已有 wcwidth |
| 9 | **PID 1 + `/etc/rc` 自启**（删假 `/init`） | 中高：让"开机就有网络/服务"成立 | ~1 天 | `process.c` 已有 create/fork/wait；`vfs.c:620` 假 /init 要删 |
| 10 | **ELF 从 VFS 执行**（先建地址空间再拷段，删两条重复 execve） | 中高：用户态的"自举"起点 | ~2 天 | `loader.c:37-69`、`syscalls.c:511-568`、`ring3.c:360-430` |

**次优（第 11-13 位，性价比也不错）**：`/proc`+`/dev/null`+`/tmp`（~1 天）；
USB HID 键鼠（~3 天，真机可用性）；PS/2 滚轮 + 4 字节包（~2 小时）。
**不建议现在做**：SMP 抢占调度（要合并三套调度器，风险高于收益）、GPU 加速、TLS。

---

## 8. 复核方法（每条结论都可 30 秒内重现）

```powershell
cd C:\Users\fanqi\Desktop\SutureOS

# 0) 行号以 HEAD 为准，不受工作树并行改动影响
git show HEAD:kernel/net/net.c | Select-String -Pattern 'NOT implemented'

# 1) 系统调用三集合对账（P0-4 的 179 项）
py -3 -c "
import re
h=open('kernel/syscall/syscall.h',encoding='utf-8').read()
defs=dict((m.group(1),int(m.group(2))) for m in re.finditer(r'#define\s+(SYS_[A-Z0-9_]+)\s+(\d+)',h))
reg=set(re.findall(r'reg\(\s*(SYS_[A-Z0-9_]+)',''.join(open('kernel/syscall/syscalls2.c',encoding='utf-8').read())))
sc=open('kernel/syscall/syscall.c',encoding='utf-8').read(); name_of={v:k for k,v in defs.items()}
for n in re.findall(r'syscall_register\((\d+)',sc): reg.add(name_of.get(int(n),'NUM_%d'%int(n)))
mapped=set(re.findall(r'\[LINUX_SYS_[A-Z0-9_]+\]\s*=\s*(SYS_[A-Z0-9_]+)',open('kernel/syscall/linux_compat.c',encoding='utf-8').read()))
print('registered',len(reg),'mapped',len(mapped),'mapped-unregistered',len(mapped-reg))"

# 2) 缺失子系统的"零命中即缺失"
git grep -nEi 'e1000|usb_|xhci|ehci|AC97|HDA|audio|sound' HEAD -- '*.c' '*.h'   # 0
git grep -nEi 'virtio_gpu' HEAD -- '*.c' '*.h'                                # 2，均为 virtio_blk 出处注释
git grep -nEi 'ASLR|canary|SMEP|SMAP|clipboard' HEAD -- '*.c' '*.h'           # 0

# 3) 死代码（定义存在、外部零调用）
git grep -nE 'deb_(remove|query|list_installed)\(' HEAD -- ':!kernel/pkg/deb.*'          # 0
git grep -nE 'dynlink_(init|load_library)\(' HEAD -- ':!kernel/syscall/dynlink.*'        # 0
git grep -nE 'helos_fifo32_' HEAD -- ':!kernel/ipc/fifo.*'                               # 仅头文件声明

# 4) 权限是空壳
git show HEAD:kernel/vfs/vfs.c | Select-String -Pattern 'st->mode = 0'                   # 784/788 写死
git show HEAD:kernel/syscall/syscalls2.c | Select-String -Pattern 'mode bits are reported'  # 1028
```

**本报告未做的事**：没有修改任何源码、没有 git 提交、没有跑构建/QEMU；所有"实测"引用
均来自仓库内已有的验收记录（README/CHANGELOG/release-notes）与静态代码，不代替一次真机回归。
