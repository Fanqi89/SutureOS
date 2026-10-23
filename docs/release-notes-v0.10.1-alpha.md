## 🚀 本次更新 (v0.10.0 → v0.10.1)

**移植的 libc 用户态跑通了。** v0.10.0 把 VimtuOS 的最小 libc 整包移植进来但卡在一个已诊断
的阻塞点（`int 0x80` 帧布局）；这一版把它修好，**C 程序的 printf/malloc/string 在 CPL=3
真的跑起来了**。

### 🔧 关键修复

`struct moos_regs` 声明 `rax` 在偏移 `+0x00`，但共用 ISR stub 的 push 顺序是
`rax, rcx, rdx, rbx, rsi, rdi, rbp, r8..r15`，所以内存里的布局是 `r15 … rcx, rax`
—— **`rax` 实际在 `+0x70`**（struct 尾部的 `vector +0x78` / `error +0x80` / `rip +0x88`
才是对的，这也是为什么原有各 handler 一直没出问题）。

`kernel/syscall/int80.c` 现在用显式偏移（`FRAME_RAX/RDI/RSI/RDX`）读参数、写返回值。
修之前 handler 看到的是 `nr=0` / `nr=-38`，libc 程序在 ring 3 里空转、没有任何输出。

### ✅ 验证：移植的 libc 真的在跑

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
int80 selftest: RESULT PASS (2/2)   （14 次调用，0 个 -ENOSYS）
```

即：用移植 libc 编译的 C 程序在 CPL=3 运行，`printf` 经 `int 0x80` → `write(2)` 落到
控制台，`malloc`/`string.h` 可用，退出码回传内核。整个过程跨了三层：crt0 → libc → 自有
ABI syscall 门。

### 📊 回归（全部不变，全绿）

自检 13/14、rtc 8/8、pipe 16/16、`ring3test` 9/9、FAT32 写+目录操作 PASS、宿主 17/17、
`AP 1 online`、RTL8139 ping、PS/2 鼠标、sudo 23/23。

### 📦 下载与体验

- 附件 `sutureos.iso`（13.06 MiB，13,690,880 字节）
- sha256：`6874d1b78697f35e18082dae37914c14436c0ce20c9fd44bbac5bcfad04dffe1`
- QEMU：
  ```
  qemu-system-x86_64 -cdrom sutureos.iso -m 512 -smp 2 \
      -device rtl8139,netdev=n0 -netdev user,id=n0
  ```
- 开机日志里就能看到 `hello from Vimtu64 C userland`；进 shell 后 `run hello` /
  `run probe` / `ring3test` / `int80` 自检

### ⚠️ 已知限制（下一步）

- 用户程序仍以 **IF=0** 运行：CPL=3 下被硬件中断打断后的返回路径（`isr_stubs.S` 对跨特权级
  帧的假设）还没修——这是"完整用户态"的必要条件，下一步就做它
- 单一页表 + U/S 位，无每进程地址空间 → 还不能 `fork`/`execve` 第二镜像
- ring 3 目前只有 `hello`/`probe` 两个程序，没有 shell、没有文件工具；
  `user/apps/` 就是扩展点（VimtuOS 的 busybox/shell 等可沿同一流水线继续移植）
- `loader.c` 的 ELF 加载路径仍未用于启动路径（现在走扁平 blob）
