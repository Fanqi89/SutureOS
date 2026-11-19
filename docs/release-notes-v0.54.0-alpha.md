## 🚀 本次更新 (v0.53.0 → v0.54.0)

**Wayland 第二块前置能力：memfd + MAP_SHARED。** 新增 `kernel/ipc/memfd.c`，注册 Linux 号
`memfd_create`(319)、把 `mmap`(9) 重新注册以支持 `MAP_SHARED` 的 fd 映射；外壳新命令 `shmtest`
跑内核自检，验收脚本 **18/18**：同一个 memfd 映射到两个不同地址，**一份写入在另一份里逐字节可见**
（共享同一批物理页），这正是 `wl_shm` pool 所需要的语义。

---

### 🧱 memfd + MAP_SHARED（v0.54.0）

| 项 | 做法 |
|---|---|
| 对象 | `memfd_create_obj()` 返回编码 fd（号段 `>=300`，与 pipe/unix 同风格）；最多 4 个对象、每个最多 64 页（256 KiB） |
| 定尺 | `memfd_resize()`（对应 `ftruncate`）按页懒分配，页来自 `loader_alloc_page()` 并清零 |
| 映射 | `memfd_map(vaddr, fd, len, writable)` 把对象的物理页按 4 KiB PTE 映射进**当前**地址空间并刷 TLB；**同一对象重复映射共享同一批页** |
| 反向操作 | `memfd_unmap()` 清 PTE；`memfd_read/memfd_write` 提供内核侧按偏移访问 |
| 系统调用 | `sys_memfd_create`(319) + `sys_mmap`(9) 支持 `MAP_SHARED`+memfd（`addr==0` 时落在用户堆顶端）；启动日志 `AF_UNIX socketpair + poll + memfd registered (4)` |

### ✅ 验收

| 项目 | 结果 |
|---|---|
| `scripts/shm_acceptance.py`（真 QEMU + 真键盘 `shmtest`） | **18/18 PASS** |
| 共享证明 | 写 map A 的 12 KiB 图案，从 map B 逐字节读回 **0 处不同**；反向写 `B[100]=0xA5` 在 `A[100]` 可见 |
| 无越界 | 只分配 3 页时 `memfd_page(fd,3) == NULL` |
| 回归 | `unix_acceptance` **18/18**、外壳 **42/42**、静态 **21/21**、交互 **13/13**、C 桌面 **9/9**、FAT32 宿主 **20/20**、内核自检 13/14、`[rust] core PASS` |

### ⚠️ 已知限制（如实记录）

1. **还没有 SCM_RIGHTS**（`sendmsg/recvmsg` 传 fd）——这是把 memfd 交给合成器的最后一块前置。
2. memfd 的 fd 在编码号段里，不在描述符表：`read/write/close` 目前不路由 memfd（只用 `mmap`/`ftruncate` 路径）。
3. 没有 `ftruncate` 的话只能靠 `memfd_resize`（内核侧）；用户态 `ftruncate` 对 memfd 尚未接线。
4. 对象/页上限（4×64 页）是原型规模，不是通用分配器；没有 `memfd` 名字、`MFD_ALLOW_SEALING` 等。
5. 自检在 ring 0 用当前 CR3 映射（同一套 PTE 路径）；**ring-3 夹具与最小 wayland server 尚未开始**。

### 📜 出处与许可（本版）

- 全部为 SutureOS 自有实现（GPL-3.0），未拷贝第三方代码。

### 📦 下载

- 附件 `sutureos.iso`（17573888 字节）
- sha256：`a69835226cb8594c9257a5485ee7bb3b67fb68a20f17ede10a2e392046298c51`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.54.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.54.0-alpha
