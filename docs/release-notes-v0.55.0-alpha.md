## 🚀 本次更新 (v0.54.0 → v0.55.0)

**Wayland 的第三块前置也齐了：SCM_RIGHTS（socket 上传 fd）。** 新增 `kernel/ipc/scm_fd.c` 与
`sendmsg`(46)/`recvmsg`(47) 两个系统调用，外壳新命令 `fdpass`：把 memfd 池通过 AF_UNIX socketpair
交给另一端，接收方拿到可用描述符并读回**同一份对象的字节**。验收 **21/21**。

> 至此 Wayland 的四个内核前置全部落地：**socketpair + poll + memfd/MAP_SHARED + SCM_RIGHTS**。
> 剩下的工作是"最小 wl server"和"用新外壳替换 C 桌面"，属于应用层，不再是内核阻塞项。

---

### 🧱 SCM_RIGHTS（v0.55.0）

| 项 | 做法 |
|---|---|
| 队列 | 每个方向一条最多 8 个描述符的小队列（数据走原有字节流，描述符单独排队） |
| `sendmsg(46)` | 解析 x86_64 `msghdr`（56 B）+ 最多 8 个 iovec（数据上限 4 KiB），写字节后遍历 cmsg，遇 `SOL_SOCKET/SCM_RIGHTS` 把描述符入队 |
| `recvmsg(47)` | 读字节并按 iovec 写回；若控制缓冲区够大且有排队描述符，就回填 `cmsghdr{len=16+4n, level=1, type=1}` + 描述符数组，并把 `msg_controllen` 写回 |
| 可传递范围 | 编码号段 IPC fd（pipe/unix/memfd）——**VFS 文件描述符明确拒绝（-EINVAL）**，不假装支持 |
| 引用计数 | 没有：接收方看到的就是同一个编码号（单用户内核够用），如实写在文档里 |

### ✅ 验收

| 项目 | 结果 |
|---|---|
| `scripts/fdpass_acceptance.py`（真 QEMU + 真键盘 `fdpass`） | **21/21 PASS** |
| 传递链 | memfd 建池→写 payload→socketpair→sendmsg(5B + SCM_RIGHTS)→recvmsg 拿到 1 个描述符→用它读回 payload 逐字节一致→队列排空 |
| 诚实拒绝 | 控制台 fd 不能传递（`ok   a console fd is not passable`） |
| 三件套回归 | `unix_acceptance` **18/18**、`shm_acceptance` **18/18**、`fdpass_acceptance` **21/21** |
| GUI/存储回归 | 外壳 **42/42**、静态 **21/21**、交互 **13/13**、C 桌面 **9/9**、FAT32 宿主 **20/20**、内核 13/14 |

### ⚠️ 已知限制（如实记录）

1. 描述符传递沿用编码号（无 refcount、接收方看到同一号码）；**VFS fd 不能传**。
2. `sendmsg/recvmsg` 只支持单 iovec 集合 ≤8、数据 ≤4 KiB、单次 ≤4 个描述符；没有 `MSG_PEEK/WAITALL` 语义。
3. 用户态 `ftruncate(memfd)` 仍未接线（内核侧 `memfd_resize` 可用）。
4. 自检仍跑在 ring 0（走的是 syscall 背后的同一套 helper）；**ring-3 夹具与最小 wl server 尚未开始**。
5. 目标第 ③ 项（用新外壳替换 C 桌面 `startgui`）仍未开始。

### 📜 出处与许可（本版）

- 全部为 SutureOS 自有实现（GPL-3.0），未拷贝第三方代码。

### 📦 下载

- 附件 `sutureos.iso`（17614848 字节）
- sha256：`ee623ef75bcfaed9b1c6c4da29ef33e9e1b37bbcd43eda7e54c39b26fe55f95c`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.55.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.55.0-alpha
