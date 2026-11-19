## 🚀 本次更新 (v0.55.0 → v0.56.0)

**四个 Wayland 前置能力第一次从 ring 3 跑通。** 新增 ring-3 探针程序 `user/apps/wlprobe.c`：
在**真用户态**里创建 AF_UNIX socketpair、建 memfd、`mmap(MAP_SHARED)`、把 fd 用 SCM_RIGHTS 交给对端，
再读回同一份数据。验收 **14/14**。至此这些能力不再只是"内核自检通过"，而是"用户程序真的能用"。

---

### 🧱 ring-3 探针（v0.56.0）

| 项 | 做法 |
|---|---|
| 程序 | `user/apps/wlprobe.c`（自动被 build.py 发现并嵌成 blob），`run wlprobe` 启动 |
| 系统调用通道 | Vimtu64 `int 0x80` ABI 新增 5 个**探针号**（不是 POSIX）：`10 socketpair / 11 memfd_create(size) / 12 map_shared(fd,len) / 13 send_fd(sockfd, (len<<16)|fd, buf) / 14 recv_fd(sockfd, int*outfd, buf)` |
| 3 参数限制 | 中断帧只可靠携带 `rdi/rsi/rdx`（stub 不保存 r10），所以长度打包进第二个参数——这条写进内核注释，不假装是通用 ABI |
| 结果 | socketpair / memfd / MAP_SHARED / 共享写可见 / SCM_RIGHTS 传递 / 数据逐字节 / 描述符可用，全部在 user mode 通过 |

### ✅ 验收

| 项目 | 结果 |
|---|---|
| `scripts/wlprobe_acceptance.py`（真 QEMU + 真键盘 `run wlprobe`） | **14/14 PASS** |
| 四个前置回归 | `unix_acceptance` **18/18**、`shm_acceptance` **18/18**、`fdpass_acceptance` **21/21**、`wlprobe` **14/14** |
| GUI/存储回归 | 外壳 **42/42**、静态 **21/21**、交互 **13/13**、C 桌面 **9/9**、FAT32 宿主 **20/20**、内核 13/14 |

### ⚠️ 已知限制（如实记录）

1. 那 5 个号码是**探针 ABI**（不是 POSIX socket 接口）：没有 `sockaddr`、没有 `bind/listen/accept`、没有 `sendmsg` 的完整 msghdr。
2. 长度只能打包在参数里（中断帧不带 r10）；单次数据 ≤4 KiB、单次 1 个描述符。
3. 描述符沿用编码号（无 refcount）、VFS fd 不可传（这条是 v0.55 的限制，未变）。
4. **最小 wl server（wl_compositor/wl_shm/wl_seat）尚未开始**；用户态 `ftruncate(memfd)` 未接线。
5. 目标第 ③ 项（用新外壳替换 C 桌面 `startgui`）仍未开始。

### 📜 出处与许可（本版）

- 全部为 SutureOS 自有实现（GPL-3.0），未拷贝第三方代码。

### 📦 下载

- 附件 `sutureos.iso`（17627136 字节）
- sha256：`5e1e63cb0830ce38ddba04d2e7d7175a6d0d9c944225454d46440725ee0db0aa`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.56.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.56.0-alpha
