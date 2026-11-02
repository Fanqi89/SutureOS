## 🚀 本次更新 (v0.16.0 → v0.17.0)

**系统卷能写、内存盘能装东西了：ext2 从只读驱动变成真正可写（建文件/建目录/删/改名/
大文件写），ramfs 从"64 个节点 + 单文件 2 KiB"升级成"512 节点 + 单文件 256 KiB
（堆上按 4 KiB 块增长）"。启动呈现与 GUI 外观不变。**

---

### 📦 移植/改造 1：ext2 可写（源码主导，算法参考素材库）

| 项目 | 内容 |
|---|---|
| 算法参考 | **XJ380** `driver/fs/ext4/ext4.cpp`（Apache-2.0）、**Deshab-OS** `CODE/dsk/ext4.c`（Apache-2.0） |
| 落点 | `kernel/fs/ext2.c`（1078 → **2958** 行）、`ext2.h`（415）、`ext2_vfs.c`（516）、`kernel/main/kmain.c`(+13/−5) |
| 为什么 | ext2 一直是只读驱动：系统卷能挂载、能读，但**一个文件都写不了**，等于装不了系统 |

- **分配器**：块位图 + inode 位图，组描述符与超级块的空闲计数在每次写后按位图**重算并落盘**
  （宁可慢，也不留坏位图）；`ext2_bitmaps_consistent()` 开机自检。
- **数据路径**：直接块 + 单间接 + 双间接（三间接**如实**返回 ENOTSUP，不静默丢块）、
  追加/覆盖写、`truncate`（缩到最后一个数据块并释放空表、扩大补零、读路径仍识别空洞）。
- **目录与元数据**：`create`、`mkdir`（写 `.`/`..`、父 nlink+1）、`rmdir`（非空检查）、
  `unlink`、`rename`（同目录 + **跨目录**、POSIX 覆盖语义、目录 `..` 改写、祖先环检查）。
- **VFS 适配**：默认读写挂载，`open(O_CREAT/O_TRUNC/O_APPEND)`、`write`、`mkdir/rmdir/unlink/
  rename`、`sync`、`df` 实测计数。

### 📦 改造 2：ramfs 容量（64 节点 / 2 KiB → 512 节点 / 256 KiB）

| 项目 | 内容 |
|---|---|
| 落点 | `kernel/vfs/vfs.h`、`kernel/vfs/vfs.c` |
| 为什么 | 根文件系统是内存盘，但只有 **64 个节点**、每个文件写死 **2048 字节**：
  `/etc` 种子种完就快满、写个 3 KB 文件直接 ENOSPC、`touch` 几十个文件就失败 |

- 节点池 **64 → 512**，单目录子项上限 **64 → 128**（独立限制，防止一个目录吃光整个池）。
- 文件数据从"节点内联 2048 字节数组"改成**堆上按 4 KiB 块增长**（上限 256 KiB/文件），
  首次写才分配、`rm` 时释放；`df` 按实际分配的块统计。
- 自检新增：一次写 **64 KiB（16 个块）** 随机模式并逐字节读回、**100 个文件**创建成功。

---

### ✅ 验收（真实 QEMU + 宿主独立解析）

| 项目 | 结果 |
|---|---|
| ext2 写自检（1 KiB 块） | `[ext2] write self-test PASS (create=1 write=1 readback=1 indirect=1 rename=1 unlink=1 rmdir=1)` + `[ext2] bitmaps consistent` |
| ext2 VFS 自检 | `[ext2] vfs write self-test PASS (open=1 write=1 read=1 truncate=1 mkdir=1 rename=1 unlink=1 rmdir=1)` |
| 宿主校验（1 KiB） | `verify_ext2_write.py` → **44/44**（276 块双间接产物逐字节、无孤儿 inode、无重复块、父 nlink、空闲计数、原始文件未变） |
| 宿主校验（4 KiB） | 4 KiB 块镜像同样三行自检 PASS → **44/44** |
| 二次启动 | 同一张脏镜像再启动再验 → **44/44**（自检可重复、不泄漏） |
| ext2 只读回归 | 干净镜像 `verify_ext2_read.py` → **47/47**，4 KiB **35/35**，三间接夹具 **16/16** |
| ramfs 容量 | `[ramfs] capacity self-test: big=65536 bytes chunks=16 files=100 nodes=104/512`，`[selftest][xj_vfs] PASS` |
| shell 可见 | `df` 显示 `xj_vfs ram 131072 1K-blocks`；`mkdir /cap` + `touch /cap/a.txt` + `ls` 正常 |
| FAT32 回归 | 长名写自检 PASS、宿主 `verify_fat32_write.py` **21/21** |
| 网络回归 | `scripts/nic_acceptance.py` 两块网卡 PASS |
| 外观门禁 | `gui_appearance_check.py ... --live-shot ...` → 动态矩形外 **0 像素** |
| 自检 / PANIC | `13/14`（`moos_intr` 既有项）、无 PANIC |

### 🧪 本版新增脚本

- `scripts/verify_ext2_write.py`（711 行）：**不信任客人输出**的独立 ext2 解析器——位图/组描述符
  计数、inode 块指针（直接+单+双间接）、目录链、nlink、孤儿/重复块全查。

### ⚠️ 已知限制（如实记录）

1. **ext2 三间接块写入拒绝**（1 KiB 下实测在逻辑块 65804 返回 ENOTSUP）；用了三间接指针的
   文件 `unlink`/`truncate` 也会返回 ENOTSUP（不会静默丢块）。
2. **只写主超级块/GDT**，备份副本不更新（mount 时打印说明；`e2fsck` 可从主副本重建）。
3. **4 KiB 块的 4 MiB 测试卷太小**（919 个空闲块），双间接写覆盖不到，双间接只在 1 KiB 卷上验证。
4. **每次写都全量重算位图 + flush sb/GDT**，所以挂 ext2 测试盘时开机 ext2 阶段明显变慢
   （QEMU 需等 ≥30 s）；不挂 ext2 盘没有额外开销。
5. inode 时间戳不更新；空洞写一律分配补零；无 symlink/硬链接/xattr；文件上限 4 GiB。
6. 自检只在测试镜像上跑（`/HELLO.TXT` 精确匹配才进入），真实用户卷不会被写。
7. ramfs 文件上限 256 KiB、节点 512（含目录）；这是内存盘，重启仍会丢（持久化要等"安装器"批次）。

### 📜 出处与许可（本版）

- ext2 可写是**在 SutureOS 自己的 ext2 驱动上扩展**；写入算法参考 XJ380 / Deshab-OS 的
  ext4 实现（均为 **Apache-2.0**，只读参考，未拷代码）。`docs/provenance.md` 已登记。
- ramfs 扩容是 SutureOS 自有代码（GPL-3.0）。

### 📦 下载

- 附件 `sutureos.iso`（14542848 字节）
- sha256：`ee63a13174b08773c023f8006265d5cb6eabb380ddbd387a18a4e47b7f0fa32c`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.17.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.17.0-alpha
