## 🚀 本次更新 (v0.52.0 → v0.53.0)

**Wayland 第一块前置能力落地：AF_UNIX socketpair + poll(2)。**
新增 `kernel/ipc/unix_socket.c`，注册两个 Linux 号系统调用（`socketpair`=53、`poll`=7），
外壳里可以用新命令 `unixtest` 跑内核自检；验收脚本 **18/18**。

---

### 🧱 AF_UNIX socketpair + poll（v0.53.0）

| 项 | 做法 |
|---|---|
| 端点 | `unix_socketpair(fds)` 建一对双向环形缓冲（4 对、每方向 2 KiB），fd 走独立号段 `>= 200`，与 pipe 同样由 `read/write/close` 分流 |
| 语义 | 已连接双向字节流：短写友好、对端关闭后 `read()` 返回 0（EOF）、无数据且对端还开着返回 `-EAGAIN`、向无对端写返回 `-EPIPE` |
| poll | `unix_poll_fd()` 报告 `POLLIN`（有数据或 EOF）/`POLLOUT`（还有空间）/`POLLHUP`/`POLLERR`；`poll(2)` 支持最多 16 个 fd，尽力而为的超时用有界 `pause` 自旋（单任务内核不能真阻塞） |
| 系统调用 | `syscall_register(53, sys_unix_socketpair)`、`syscall_register(7, sys_unix_poll)`，启动日志 `[syscall] AF_UNIX socketpair + poll registered (2)` |
| 内核自检 | 新 shell 命令 `unixtest`：创建/双向传输/轮询就绪/EOF/POLLHUP 共 15 项断言 |

### ✅ 验收

| 项目 | 结果 |
|---|---|
| `scripts/unix_acceptance.py`（真 QEMU + 真键盘 `unixtest`） | **18/18 PASS** |
| 双向传输 | 8 字节逐字节一致、反方向 2 字节一致 |
| poll 语义 | 新对 `POLLOUT` 有 / `POLLIN` 无；写入后 `POLLIN` 出现；读空后 `POLLIN` 消失；对端关闭 `POLLHUP` |
| 回归 | 外壳 **42/42**、静态 **21/21**、交互 **13/13**、C 桌面 **9/9**、FAT32 宿主 **20/20**、内核自检 13/14、`[rust] core PASS` |

### ⚠️ 已知限制（如实记录）

1. **还没有 SCM_RIGHTS（fd 传递）**、没有 `memfd_create`/`MAP_SHARED`、没有 `bind/listen/accept` 与路径名 socket——这些是 Wayland 的下一批前置。
2. `poll()` 不是真阻塞：超时用有界自旋，没有 wait queue/调度。
3. socketpair 只支持 AF_UNIX + stream/seqpacket；`SOCK_DGRAM`、SO_PEERCRED、抽象命名空间均未实现。
4. 还没有 ring-3 侧的 socketpair 夹具（自检在 ring 0 跑，走的是同一套内核路径；Rust/Wayland 客户端要等下一批）。
5. 最小 wayland server（wl_compositor/wl_shm/wl_seat）尚未开始；C 桌面仍在用 `startgui`。

### 📜 出处与许可（本版）

- 全部为 SutureOS 自有实现（GPL-3.0），未拷贝第三方代码。

### 📦 下载

- 附件 `sutureos.iso`（17532928 字节）
- sha256：`ea40bbcbb542068bc28594677dad3866953a0495baaf438ac181b4628480e1ba`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.53.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.53.0-alpha
