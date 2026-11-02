## 🚀 本次更新 (v0.17.0 → v0.18.0)

**桌面多了一个能看图的程序（BMP/QOI 解码，界面是 SutureOS 自己画的）；ring-3 用户态第一次有
一套能用的文件工具（每个命令一个子进程）；顺手修掉一个 FAT32 真 bug：目录长到第二个簇以后，
新建的条目在查找和列目录里都“看不见”。启动呈现与 GUI 外观不变。**

---

### 📦 移植/新增 1：图片查看器（BMP + QOI 解码）

| 项目 | 内容 |
|---|---|
| 解码参考 | **HBOS** `src/bmp.c`（GPL-3.0）、**PlantOS** `src/plic/qoi.c`（MIT），重写为无 malloc / 有界解码 |
| 落点 | 新增 `kernel/gui/img_bmp.c`、`img_qoi.c`、`imgdecode.h`；`kernel/gui/apps.c` 新增应用；`gui.h`/`gui.c` 增加**懒启动**入口 |
| 为什么 | 系统完全没有图像解码能力，也没有可以打开文件的 GUI 程序 |

- **按需创建**：图片查看器**不在默认桌面**里（否则默认截图就变了）；开始菜单第一次点它时才
  `gui_create_window()`，所以冻结基线截图逐像素不变。
- **界面自绘**：标题栏/状态行/边框/配色全部沿用 SutureOS 自己的 chrome，不搬任何外部皮肤；
  状态行显示 `文件名 宽x高 格式 (位深)`，图片按整数倍最近邻缩放居中，右下显示 `scale xN`。
- **解码范围**：BMP 1/4/8/16/24/32bpp、BIFF_RGB、top-down/底行优先、越界与超大图拒绝；
  QOI 全部 opcode（RGB/RGBA/INDEX/DIFF/LUMA/RUN）+ 截断保护。

### 📦 移植/新增 2：ring-3 文件工具（8 个程序 + ushell 命令表）

| 项目 | 内容 |
|---|---|
| 落点 | `user/apps/r3tool.h` + `rls/rcat/rcp/rmv/rrm/rmkdir/rtouch/rdf.c`；`kernel/syscall/{ring3,syscalls,syscalls2,syscall.h}` |
| 为什么 | ring-3 只有 shell/hello/probe 三个演示程序，**一个文件都碰不到** |

- 8 个工具直接调 Linux 号系统调用，失败返回真 `-errno`（`ENOENT/ENOTEMPTY/EXDEV/EEXIST`）。
- 内核侧补齐三处**真实缺陷**：目录 fd（`open()` 目录 + `getdents64`，`FD_FLAG_DIR`、read/write
  返 `EISDIR`、fork 不继承游标）、`execve` 的 argv 启动帧（`"name arg1 arg2"` →
  `[argc][argv][NULL][envp]`，`crt0.S` 识别）、ring-3 fd 从 3 起分配（否则第二次 `open` 会
  占掉 stdout，`printf` 写进被复制的文件）。
- `ushell` 用 `fork + execve + wait4` 跑每条命令（每个命令一个子进程，shell 不被替换）。

### 🔧 关键修复：FAT32 多簇目录的“目录结尾”判定

- **现象**：`lfn-write self-test` 里 rename 返回成功、但新名字 `stat` 打不开，随后 unlink 也失败，
  盘上留下孤儿簇。只在目录条目超过一个簇（16 条）时出现。
- **根因**：`dir_find` / `dir_find_lfn` / `readdir` / `rmdir` / unlink 的 LFN 链扫描都把
  **第一个 `0x00` 槽当作目录结尾**。目录长大以后，老簇尾部仍是 `0x00`，新条目却在下一个簇里，
  于是“新建的条目永远找不到”。
- **修法**：`0x00` 只有在**链的最后一个簇**才算结尾（新增 `dir_cluster_is_last()` 查 FAT 链），
  否则跳过继续扫。

### ✅ 验收（真实 QEMU + 宿主独立校验）

| 项目 | 结果 |
|---|---|
| 图片查看器（BMP） | 真实鼠标点开始菜单 → `Image Viewer` → 窗口创建、`[img] decoded /mnt/fat/IMG.BMP 32x32 bmp bpp=24`，截图四象限像素 **逐个精确匹配**（红/绿/蓝/白） |
| 图片查看器（QOI） | 窗口内点击切到 QOI → `[img] switched to /mnt/fat/IMG.QOI (32x32)`，四象限像素同样精确匹配 |
| ring-3 工具 | 在 `port/r3tools` 分支上：`ushell` 一条会话 22 条命令自我断言 **37/37 PASS**（rls/rcat/rmkdir/rtouch/rcp/rmv/rrm/rdf + 错误路径 ENOTEMPTY/EXDEV），宿主 `verify_fat32_write.py` **21/21**，无 PANIC、无 user fault。合并进 main 后重跑了 ushell + `rls /mnt/fat`（目录列表正确输出）；同一套工具的完整 22 条脚本在这台机器上含交互时序，合并树里只做了冒烟，没有把 37/37 当作合并树的成绩 |
| FAT32 多簇目录 | `[fat32] lfn-write self-test PASS (create=1 readdir=1 rename=1 unlink=1 utf8=1)`；宿主 21/21（含“根目录链已分配且正确终止”“无孤儿簇”） |
| 回归 | 自检 13/14、`run probe/hello/forktest` PASS、`git diff` 未触及 term/fbcon/启动路径 |
| 外观门禁 | 默认桌面仍是 3 个窗口（图片查看器懒创建），`gui_appearance_check.py` 动态矩形外 **0 像素** |

### 🧪 本版新增脚本

- `scripts/imgview_acceptance.py`：QMP 真实鼠标点菜单 → 截图 → 像素断言（含 QOI 切换）。
- `scripts/ring3_tools_acceptance.py`：22 条 ring-3 命令的自动驱动 + 断言 + 宿主校验 + 前后目录对照。
- `scripts/mkfat32_testimg.py`：新增 `IMG.BMP`（24bpp 四象限）与 `IMG.QOI`（QOI 编码同图）夹具。

### ⚠️ 已知限制（如实记录）

1. **图片查看器**：只读文件、无文件浏览器（冻结 chrome 没有列表控件），来源是固定候选路径
   `/mnt/fat/IMG.BMP`、`/mnt/fat/IMG.QOI`、`/images/*`；窗口内点击在两张图间切换。最大
   600×480 像素（解码缓冲上限），超出拒绝；不缩放为“平滑插值”（最近邻整数倍）。
2. **ring-3 工具**：`run <name>` 不能传参（带参数走 `ushell` 的 fork+execve）；`-r` 递归删除
   如实拒绝；跨目录 rename 被 VFS 拒绝（EXDEV）；`touch` 不改 mtime（VFS 无 mtime 设置）；
   `rdf` 报的是 SutureOS 方言的 statfs（两个 u64 字节数），不是 Linux `struct statfs`。
3. FAT32 多簇目录已修，但**目录项跨簇的删除**只保证“链标记正确”，没有做“整个簇空出来后回收”
   的优化（簇会留着当目录空间）。

### 📜 出处与许可（本版）

- BMP 解码：**HBOS** `src/bmp.c`（GPL-3.0）；QOI 解码：**PlantOS** `src/plic/qoi.c`（MIT）——
  都是**算法参考重写**，文件头写明来源/路径/许可证。
- ring-3 工具：SutureOS 自有实现（GPL-3.0），argv 启动帧/目录 fd 的 ABI 约定参考 VimtuOS（GPL-3.0）。

### 📦 下载

- 附件 `sutureos.iso`（14723072 字节）
- sha256：`1affe846cad45a46afc75847a2d32565722d4ad61f97bc3242b87fc33e09ccd5`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.18.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.18.0-alpha
