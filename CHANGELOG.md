# Changelog

All notable changes to SutureOS are documented here.
The version numbers follow [Semantic Versioning 2.0.0](https://semver.org/) and
every release is cut as an annotated git tag `v<version>` whose message also
describes the release. Tags are never moved or deleted, so all versions coexist
in the repository.

## [v0.28.0-alpha]

**新增 `sync`：写盘不再必须 umount（FAT32 FSInfo / ext2 计数是延迟写的）。验收刻意不卸载，
宿主解析原始镜像逐字节确认。启动呈现与 GUI 外观未变。**

### Added
- `kernel/shell/shell.c`：`shell_cmd_sync()` —— 遍历 `vfs_list_mounts()` 逐个 `vfs_sync()`，
  逐卷报告结果与汇总，失败返回非 0。
- `scripts/sync_acceptance.py`：write → sync → cat（不 umount）+ 宿主侧 FAT32 逐字节校验。

### Not shipped (honest)
- ext2 多卷尝试：结构改动可跑通（两卷同挂、各自读写），但宿主写验证器在卷 1 报
  `artifact content byte-exact` 失败，怀疑多卷切换下写路径串卷；根因未查明，**已整体回退**，
  记录在 docs/porting-roadmap.md B12。

### Verification
- guest `sync: /mnt/fat (fat32) ok` + `sync: 1 ok, 0 failed`；宿主 `SYNC.TXT` 内容逐字节一致
  → **6/6 PASS**；内核 13/14、FAT32 宿主 21/21、GUI 外观门禁 0 像素。

## [v0.27.0-alpha]

**Intel HDA 音频（B9）：控制器 + CORB/RIRB verb + codec 枚举 + 音频输出路径 + 环形 BDL 播放，
QEMU 里真实出声；宿主侧解析 WAV 证明 48 kHz/2ch/16bit、1.002 s、0.5000 FS、440.01 Hz。
启动呈现与 GUI 外观未变。**

### Added
- `kernel/drivers/hda.c`（1236 行）+ `hda.h`：PCI 探测 / BAR0 MMIO / GCTL 复位 / CORB-RIRB DMA verb /
  codec-AFG-widget 枚举 / 输出引脚→转换器路径 DFS / amp+EAPD+PIN_OUT_ENABLE / SD0 环形 BDL 播放 /
  正弦合成；骨架 PlantOS（MIT），改动逐条记在文件头。
- `scripts/verify_hda_wav.py`：宿主侧 WAV 校验（采样率/声道/位深/时长/峰值/频率）。
- `kmain.c` 接入（无设备只多一行日志）；shell 新增 `hdatest [freq] [ms]`。

### Fixed / learned (QEMU 实测)
- CORB 第一条命令必须写在索引 1（复位后 WP=0，写回 0 无命令）。
- RINTCNT 是每批响应数；必须 RINTCNT=1 + RIRB 响应状态位 + 每条响应 W1C 清 RIRBSTS。
- SD_CTL 的 RUN 是 bit1(0x02)；stream tag 在 bits23:20。
- GET_PARAMETER id 与输出 SD0 基址（0x100）按规范取值。

### Verification
- 有 HDA：codec 枚举 → 播放 → `selftest: PASS played 48000 frames @48000 Hz`；
  宿主 WAV **6/6 PASS**（48000/2ch/16bit/1.002 s/0.5 FS/440.01 Hz）。
- 无 HDA：模板差异仅 `+[hda] no controller`；13/14 自检、FAT32 21/21、外观门禁 0 像素；警告零新增。

### Limits
- 仅 QEMU 验证（真机未测）；单 codec/单输出 pin；无采集、无音量控件、无中断；
  固定 48 kHz/16-bit/立体声；单次 ≤1 s；WAV 含 600 ms drain 静音尾巴；有 HDA 时开机 +1~2 s。

## [v0.26.0-alpha]

**命令行补齐五个文本工具：`wc` / `head` / `tail` / `grep` / `stat`（全部走公开 VFS，四种文件系统
一致可用）；验收用 shell 自己造多行文件再逐条断言，宿主侧解析 FAT32 镜像逐字节复核。
启动呈现与 GUI 外观未变。**

### Added
- `kernel/shell/shell.c`：`wc`（行/词/字节流式统计）、`head`（前 n 行）、`tail`（最后 n 行，
  偏移窗口 + vfs_seek 回读）、`grep`（字面子串 + 行号 + 命中计数）、`stat`（类型/大小）。
- `scripts/textutils_acceptance.py`：QMP 注入 10 条命令 + 串口断言 + 宿主侧 FAT32 逐字节校验。

### Verification
- `wc` → `5 10 53`；`head 2` 只前两行；`tail 2` 只后两行；`grep alpha` → 2 处带行号；
  `stat` → regular file / 53 bytes → **7/7 PASS**；宿主 `TXT.TXT` 逐字节一致。
- 回归：内核 13/14、FAT32 宿主 21/21、无 PANIC、GUI 外观门禁 0 像素。

### Limits
- grep 非正则、区分大小写、无 -i/-v；单行 >512 字节截断；tail 窗口 64 行；无管道；tail 扫两遍。

## [v0.25.0-alpha]

**第五个桌面应用：Text Editor —— 开始菜单打开、真实键盘输入、回车保存到 /mnt/fat/EDIT.TXT，
宿主解析 FAT32 镜像逐字节验证。启动呈现与默认桌面外观未变。**

### Added
- `kernel/gui/apps.c`：`APP_ID_EDT=7` 文本编辑器（加载/插入/退格/换行保存/光标/状态行，
  1 KiB 缓冲，SutureOS 自绘）；开始菜单新增 `Text Editor`（懒创建，默认桌面不变）。
- `scripts/editor_acceptance.py`：真实点击 + 键盘注入 + 宿主侧 FAT32 逐字节校验。

### Fixed
- GUI 的 `q`/`r` 全局快捷键会在文本窗口抢走正文输入（"editor" 丢 r、q 直接退出桌面）：
  现在只在没有文本输入窗口获得焦点时生效；Ctrl+S(0x13) 也转发给 `on_key`。

### Verification
- 点菜单 → 窗口创建 → 输入 `hello editor 42` + 回车 → `[edit] saved 17 bytes` →
  宿主 `EDIT.TXT` = `hello editor 42\n` 逐字节相同 → **5/5 PASS**。
- 回归：内核 13/14、FAT32 宿主 21/21、图片查看器 + GUI 中文 4/4、外观门禁 0 像素。

### Limits
- 回车即保存（解码器不报 CTRL）；固定文件路径；无打开/另存为、无方向键编辑、无撤销/粘贴、
  1 KiB 缓冲、无 CJK 输入。

## [v0.24.0-alpha]

**FAT32 驱动支持多卷：`/mnt/fat`（IDE）与 `/mnt/usb`（U 盘）同一次启动里同时可读可写，
移除 v0.23.0 里如实标注的"fatfs 单卷"限制；宿主解析两个镜像证明写入落在正确卷。
启动呈现与 GUI 外观不变。**

### Changed
- `kernel/fs/fatfs.c`：单卷 `g_vol` → `g_vols[4]` + 当前卷指针；新增
  `fatfs_select_volume/fatfs_current_volume/fatfs_volume_count`；`fatfs_mount()` 返回卷槽号；
  `fatfs_unmount(drive)` 按驱动器清槽并关闭该卷句柄；文件/目录句柄记录所属卷。
- `kernel/fs/fatfs_vfs.c`：挂载点表 `g_mnts[4]` + 前缀匹配 `vol_path()`，每操作先选卷。
- `kernel/main/kmain.c`：USB 挂载段的卷限制文案更新。

### Added
- `scripts/multivol_acceptance.py`：双卷同挂 + 交叉写 + 宿主侧两镜像逐字节校验。

### Verification
- 双卷同挂并各自 `ls`/`cat` 正确；`/mnt/fat` 挂着时写 `/mnt/usb/mv.txt` 成功 → 宿主
  U 盘镜像内容逐字节一致、IDE 卷内容不变 → **12/12 PASS**。
- 单卷回归：FAT32 宿主 21/21、USB 宿主 5/5、内核 13/14、终端中文 10/10、wget 8/8、
  GUI 外观门禁 0 像素。

### Limits
- 卷槽上限 4；挂载点不可嵌套；句柄表仍全局 8+8；ext2/squashfs 仍单卷；无新增并发锁。

## [v0.23.0-alpha]

**第一次把 U 盘当盘用：EHCI（USB 2.0）主控 + 高速枚举 + BOT/SCSI 读写 → 块设备 drive 16 →
FAT32 自动挂 `/mnt/usb`，可 `ls`/`cat`/`write`；宿主侧解析 U 盘镜像逐扇区逐字节验证。
启动呈现与 GUI 外观未变。**

### Added
- `kernel/drivers/usb_ehci.c`（1430 行）+ `usb_ehci.h`：EHCI 端口复位/枚举、异步 QH/qTD、
  BOT、SCSI（INQUIRY/READ CAPACITY/READ(10)/WRITE(10)/TUR/SYNC CACHE）、`blk_register` drive 16、
  开机自检。骨架 HanOS（MIT），BOT 流程参照 HBOS（GPL-3.0），改动见文件头。
- `block.h`：`BLK_TYPE_USB=7`、`BLK_DRIVE_USB=16`；`kmain.c`：`usb_probe()` + `/mnt/usb` 挂载/自检。
- `scripts/mk_usb_testimg.py`、`scripts/verify_usb_disk.py`（宿主侧独立校验）。
- `scripts/mkfont_cjk.py`：字体路径加绝对候选，修掉 git worktree 下构建失败。

### Fixed
- PORTSC 偏移漏加 CAPLENGTH → 读到全 0（看起来像没插设备）。
- CBW/CSW 与数据阶段共用 bounce buffer → CSW 覆盖数据（INQUIRY vendor 空、READ CAPACITY 异常）。
- 高速判据不读 QEMU 恒为 0 的 `PORTSC.PSPD`，改用"复位释放后 PED=1"。

### Verification
- 有 USB：枚举→`storage: 71680 blocks x 512`→`mount /mnt/usb OK`→`selftest PASS`；`ls` 4 条目；
  `write`+`cat`+`umount` 后宿主 `verify_usb_disk.py --allow-write` **8/8**（6 扇区变化全在合法区域、
  W.TXT 逐字节正确、原文件未变）。
- 只读启动整盘 CRC32 `93F4BB22` 未变 → 5/5。
- 无 USB：只多 `[usb] no controller`；13/14 自检、FAT32 21/21、wget 8/8、终端中文 10/10、
  GUI 中文 4/4、外观门禁 0 像素。

### Limits
- 仅高速设备；第一台 mass storage、LUN 0；仅开机枚举无热插拔；无 EHCI HID；fatfs 单卷
  （同时挂 IDE FAT32 时 `/mnt/usb` skipped 并如实失败）；USB CRC 错误路径未验。

## [v0.22.0-alpha]

**第一次能把网上的文件下载进本地盘：`wget http://<ipv4>[:port]/<path> [outfile]`（HTTP/1.0 GET
over 内核 TCP 客户端，边收边写 VFS，Content-Length 校验）。宿主起真实 HTTP 服务器、事后解析
FAT32 镜像逐字节比对相同。启动呈现与 GUI 外观未变。**

### Added
- `kernel/shell/shell.c`：`shell_cmd_wget()` —— URL 解析、HTTP 请求/响应头处理、`Content-Length`
  校验、流式写盘；只接受 IPv4 字面量。
- `scripts/wget_acceptance.py`：宿主 ThreadingHTTPServer + QMP 注入 + 宿主侧 FAT32 解析比对。

### Verification
- guest：`HTTP 200, content-length=3072` → `wrote 3072 bytes`；文本文件 `cat` 内容一致。
- 宿主：`DL.BIN` sha256 与服务端文件完全相同；`HELLO2.TXT` 逐字节相同 → **8/8 PASS**。
- 回归：内核自检 13/14、FAT32 三自检 PASS、终端中文 10/10、GUI 中文 4/4、外观门禁 0 像素。

### Limits
- 仅 IPv4 字面量；HTTP/1.0 GET only（无 HTTPS/POST/重定向/chunked/续传/代理）；响应头 2 KiB
  上限；超时 5 秒；无进度显示。USB 存储在独立分支开发中。

## [v0.21.0-alpha]

**中文从终端走进窗口：GUI 文本绘制支持 UTF-8 与 16×16 点阵（复用 GB2312 子集），图片查看器
底部显示 `缩放 xN`，宿主截图逐像素断言。启动呈现与默认桌面外观未变。**

### Added
- `kernel/gui/gui.c`：`gui_draw_text()` 增加 UTF-8 解码与宽字符 16×16 点阵绘制（两格宽，走
  既有 `gui_px()` 裁剪）；字体子集复用 `kernel/term/cjk16.c`。
- 图片查看器页脚 `scale xN` → `缩放 xN`（新建窗口，不影响默认桌面）。
- `scripts/gui_cjk_acceptance.py`：QMP 点开始菜单 → Image Viewer → 截图 → 按页脚颜色与
  16×16 点阵精确匹配“缩/放”。

### Verification
- GUI：checks=4 ok=4（缩(161,402)、放(177,402) 逐像素相同）。
- 终端回归：10/10（`ls` 中文名 + 滚动后仍精确）。
- 回归：内核自检 13/14、FAT32 三自检 PASS、无 PANIC；外观门禁 0 像素。

### Limits
- 仅新建窗口内中文；窗口标题/开始菜单/默认应用仍英文（冻结外观）。
- GB2312 子集、位图 16×16 不缩放、无输入法、GUI 文本不折行。

## [v0.20.0-alpha]

**终端第一次能显示中文：GB2312 全集 16×16 点阵（7443 字形，SIL OFL-1.1 字体，构建期渲染），
UTF-8 流式解码 + 双宽排布 + 滚动安全的覆盖层；宿主在截图上按 16×16 逐像素断言。
启动呈现与 GUI 外观未变。**

### Added
- `kernel/term/cjk16.c/.h`：16×16 点阵 blob 解析 + 二分查找 + framebuffer 贴图。
- `scripts/mkfont_cjk.py`：构建期从 OFL 字体渲染 GB2312 全集 → `build/font_cjk.bin`（构建自动执行）。
- `kernel/term/term.c`：UTF-8 流式解码、宽字符双空格排布、512 条覆盖层（滚动上移重绘）、
  flush-before-blit 顺序；`flanterm_backends/fb.c/.h` 只读 metrics 访问器。
- `scripts/cjk_terminal_acceptance.py`：启动 + `ls` + 截图 + numpy 全屏字形匹配 + 滚动复验。
- `docs/licenses/OFL-1.1.txt` + provenance/licensing-audit 字体条目。

### Fixed
- 中文首版渲染成实心块/滚动后丢失：点阵写在 framebuffer 后仍 flush 双缓冲栅格，空格被重画；
  → 先 flush 再贴像素，滚动时覆盖层整体上移并重绘。
- 多字节 UTF-8 序列在 term_putc/term_write 之间被拆散：→ term.c 统一流式解码。

### Verification
- 宿主像素断言：`中文长名` 四字形与点阵逐像素相同（中(3,368) 文(21,368) 长(48,368) 名(66,368)）；
  30 回车滚动后仍逐像素相同、整体上移整 7 行 → **10/10 PASS**。
- 回归：内核自检 13/14、FAT32 三自检 PASS、无 PANIC、GUI 外观门禁 0 像素。

### Limits
- 仅 GB2312（7443 字形）；位图固定 16×16；GUI 尚无中文；覆盖层 512 字形、只跟踪滚动；
  中文输入不支持。

## [v0.19.0-alpha]

**终端第一次能自己写文件（`write` / `append` / `hexdump`），只用公开 VFS API，ramfs/FAT32/ext2
一致生效；内容由宿主侧独立解析镜像逐字节核对。启动呈现与 GUI 外观未变。**

### Added
- `write <file> <text...>`：创建/整体替换文本文件，自动补行尾换行，回显字节数。
- `append <file> <text...>`：追加一行（读出 → 补换行 → 整文件写回），回显新大小。
- `hexdump <file> [offset] [length]`：偏移 + 16 字节 hex + ASCII 视图，支持十进制/0x 参数。
- `scripts/shell_text_tools_acceptance.py`：QMP 注入三条命令 + 8 条断言 + 宿主 FAT32 解析。

### Verification
- guest：`write ... (23 bytes)`、`append ... now 36 bytes`、`cat` 两行、`hexdump` 偏移+hex+ASCII。
- 宿主：`NOTE.TXT` 内容与 `"hello shell text tools\nsecond line\n"` 逐字节相同，原夹具文件仍在。
- 回归：内核自检 13/14、FAT32 三自检 PASS、无 PANIC、GUI 外观门禁 0 像素。

### Limits
- write/append 是整文件重写（VFS 无 append 标志），append 上限 2 KiB、write 单行 512 字节。
- CJK 显示仍未进版：本轮做了字体子集 + flanterm 宽字符钩子但渲染成实心块，已整体回退（不发半成品）。

## [v0.18.0-alpha]

**桌面多了图片查看器（BMP/QOI 解码，SutureOS 自绘界面、按需创建所以默认截图不变）；
ring-3 用户态第一次有可用的文件工具（8 个程序 + fork/execve/wait4 的子进程模型）；
修掉一个真 FAT32 bug：目录长到第二个簇以后，新建条目在查找/列目录里“看不见”。
启动呈现与 GUI 外观未变。**

### Added
- **图片查看器**（`kernel/gui/apps.c` + 新增 `img_bmp.c`/`img_qoi.c`/`imgdecode.h`）：
  BMP 1/4/8/16/24/32bpp（BIFF_RGB、top-down/底行优先）与 QOI 全 opcode，无 malloc、有界、
  600×480 上限；状态行显示 `文件 宽x高 格式 (位深)`，整数倍最近邻缩放居中；窗口内点击切换图源。
  解码算法参考 HBOS `src/bmp.c`（GPL-3.0）/ PlantOS `src/plic/qoi.c`（MIT），重写而非拷贝。
- **懒启动**（`gui.h`/`gui.c`/`apps.c`）：开始菜单第一次点图片查看器才 `gui_create_window()`，
  默认桌面仍是 3 个窗口，冻结基线截图逐像素不变。
- **ring-3 文件工具 8 个**：`rls/rcat/rcp/rmv/rrm/rmkdir/rtouch/rdf`（`user/apps/r3tool.h` 薄封装，
  直调 Linux 号系统调用、返回真 -errno）；`ushell` 命令表扩展。
- **内核 ring-3 基础设施三处补齐**：目录 fd + `getdents64`（`FD_FLAG_DIR`、read/write 返 EISDIR、
  fork 不继承游标）、`execve` 的 argv 启动帧、ring-3 fd 从 3 起分配。
- 脚本：`scripts/imgview_acceptance.py`、`scripts/ring3_tools_acceptance.py`；`mkfat32_testimg.py`
  新增 `IMG.BMP`/`IMG.QOI` 夹具。

### Fixed
- **FAT32 多簇目录的目录结尾判定**：`dir_find`/`dir_find_lfn`/`readdir`/`rmdir`/unlink 的 LFN 扫描
  都把第一个 `0x00` 槽当目录结尾；目录长大到第二个簇后，老簇尾部全是 `0x00`，新条目永远扫不到
  （表现为 rename 报成功但新名字打不开、unlink 失败、留下孤儿簇）。新增 `dir_cluster_is_last()`
  查 FAT 链，只有链的最后一个簇的 `0x00` 才算结尾。
- `scripts/verify_fat32_write.py` 同步：读整条根目录链、接受合法的多簇根目录、孤儿簇统计
  把整条根链算作 live。

### Verification
- 图片查看器：真实鼠标（QMP）点开始菜单 → Image Viewer → `[img] decoded /mnt/fat/IMG.BMP 32x32 bmp bpp=24`，
  截图四象限像素精确匹配；点击切换 QOI → `[img] switched to /mnt/fat/IMG.QOI (32x32)` 同样匹配。
- ring-3 工具：`ushell` 22 条命令自断言 **37/37**；宿主 `verify_fat32_write.py` **21/21**；
  无 PANIC、无 user fault；`run probe/hello/forktest` 仍 PASS。
- FAT32：lfn-write 自检 PASS（含 rename/unlink），宿主 21/21，根目录链合法（fat[2]=0x1b）、0 孤儿簇。
- 回归：内核自检 13/14、FAT32 三个自检 PASS、GUI 默认桌面 3 窗口、外观门禁动态矩形外 **0 像素**。

### Limits
- 图片查看器无文件浏览器（冻结 chrome 无列表控件），固定候选路径；最大 600×480；最近邻缩放。
- ring-3：`run` 不能传参；`-r` 递归删除如实拒绝；跨目录 rename 被 VFS 拒绝；touch 不改 mtime；
  `rdf` 是 SutureOS 方言 statfs（两个 u64）。
- FAT32 多簇目录空出的整簇不回收（留作目录空间）；终端仍无 CJK 字体。

## [v0.17.0-alpha]

**系统卷能写了：ext2 从只读驱动变成真正可写（建文件/建目录/删/改名/大文件写）；
根内存盘从 64 节点 / 单文件 2 KiB 扩到 512 节点 / 单文件 256 KiB（堆上按 4 KiB 块增长）。
启动呈现与 GUI 外观未变。**

### Added
- **ext2 可写**（`kernel/fs/ext2.c` 1078 → 2958 行、`ext2.h` 415、`ext2_vfs.c` 516）：
  位图块/inode 分配（每次写后按位图重算组描述符与超级块空闲计数并落盘）、直接+单+双间接写、
  追加/覆盖/truncate（含空洞）、create/mkdir/rmdir/unlink/rename（同目录+跨目录、POSIX 覆盖、
  目录 `..` 改写、祖先环检查、nlink/used_dirs）、VFS 默认 rw 挂载 + `df` 实测计数。
  算法参考 XJ380 / Deshab-OS 的 ext4 实现（Apache-2.0，只读参考，未拷代码）。
- **ext2 写自检 / VFS 写自检 / 位图一致性自检**。
- **ramfs 扩容**（`kernel/vfs/vfs.h`、`vfs.c`）：节点 64→512、单目录子项 64→128；文件数据改为
  堆上 4 KiB 块增长（256 KiB/文件上限，rm 释放）；`df` 按实际分配统计；开机自检写 64 KiB
  并逐字节读回 + 创建 100 个文件。
- **`scripts/verify_ext2_write.py`**（711 行）：独立 ext2 解析器（位图/sb/GDT 计数、inode
  直接+单+双间接块逐字节、目录链、父 nlink、孤儿/重复块、原始文件未改动）。

### Verification
- 1 KiB ext2：写自检 PASS、位图一致、VFS 写自检 PASS；宿主 **44/44**（276 块双间接产物）。
- 4 KiB ext2：同样 PASS → 宿主 **44/44**；同镜像二次启动再验 **44/44**。
- 只读回归：干净镜像 **47/47**、4 KiB **35/35**、三间接夹具 **16/16**。
- ramfs：`[ramfs] capacity self-test: big=65536 bytes chunks=16 files=100 nodes=104/512`，
  `[selftest][xj_vfs] PASS`；shell `df` 131072 1K-blocks、mkdir/touch/ls 正常。
- 回归：FAT32 长名写 + 宿主 21/21、网卡验收两卡 PASS、自检 13/14、无 PANIC；
  GUI 外观门禁动态矩形外 **0 像素**。

### Limits
- ext2 三间接块写入/释放如实拒绝（ENOTSUP）；只写主超级块/GDT（备份副本不更新）；
  4 KiB 测试卷覆盖不到双间接；每次写全量重算位图并 flush，挂 ext2 盘时开机变慢（≥30 s）；
  时间戳不更新、无 symlink/硬链接/xattr、文件上限 4 GiB。
- ramfs 仍是内存盘（256 KiB/文件、512 节点、重启丢数据）。

## [v0.16.0-alpha]

**按 docs/porting-roadmap.md 开始批量移植：光驱数据盘能读（ATAPI + ISO9660）、系统第一次能出声
（AC'97）、FAT32 长文件名能创建/改名/删除（写侧）。启动呈现与 GUI 外观未变。**

### Added
- **ATAPI 光驱 + ISO9660 只读文件系统**：`kernel/drivers/atapi.c/.h`（PIO 轮询：
  IDENTIFY PACKET/PACKET+CDB/READ(10)/READ CAPACITY/REQUEST SENSE，注册只读 blk_device
  驱动号 12..15、2048B 块）+ `kernel/fs/iso9660.c/.h` + `kernel/fs/iso9660_vfs.c`。
  开机扫整段卷描述符集、用 El Torito 引导记录区分启动盘，自动挂 `/mnt/cd`。
  来源：Uinxed-Kernel（Apache-2.0）+ CoolPotOS（MIT）。
- **AC'97 声卡 PCM 输出**：`kernel/drivers/ac97.c/.h`，PCI 探测 + NAM codec + NABM PCM OUT
  32 条 BDL 轮询播放 16-bit 48 kHz；shell 新增 `beep`。来源：HBOS（GPL-3.0）。
- **FAT32 VFAT 长名写侧**（`kernel/fs/fatfs.c`）：自写 LFN 链（N|0x40…1 物理顺序、
  13 个 UTF-16 单元/条、短名校验和）、唯一 8.3 别名、删除/改名清链；UTF-16→UTF-8 读回，
  非 ASCII 长名可按原串打开。规则来源：PlantOS 的 ChaN FatFs 血统（MIT）。
- 验收脚本：`scripts/mkiso9660_testcd.py`、`scripts/verify_iso9660_read.py`（224 项）、
  `scripts/verify_ac97_wav.py`；`scripts/gui_appearance_check.py` 新增 `--live-shot`
  （用同一构建的第二次截图学习"活数据掩码"）。
- `docs/porting-roadmap.md`：素材库 50 仓库的可移植项分 B1..B10 批次。

### Fixed
- `fatfs_open(create)` 永远走不到长名创建分支（`path_resolve` 对不存在的长名直接 ENOENT）
  → 新增"父目录 + 长名组件"解析路径。
- unlink/rmdir 只删短名条目、长名链变孤儿 → 删除/改名时先标记整条 LFN 链。
- 长名写测试后 FSInfo 与实测差 1 簇 → 自检末尾补 `vfs_sync()`。
- 外观门禁误报：Task Manager 活数据数字超出旧动态矩形（pristine v0.15.0 也报 29 px）
  → 门禁升级为"动态矩形 + 双次运行活数据掩码"。

### Verification
- ISO9660：`mount drive=15 vol=SUTURE_CD files=4`、`selftest PASS`；三级目录 / 跨 3 块
  4680 B 文件 / 60 条目录项全部命中；宿主 `verify_iso9660_read.py` **224/224**。
- AC'97：WAV 实录 **48000 Hz / 1.000 s / 0.5000 FS / 440.01 Hz** 全 PASS。
- FAT32：`lfn-write self-test PASS (create=1 readdir=1 rename=1 unlink=1 utf8=1)`；
  宿主 `verify_fat32_write.py` **21/21**。
- 回归：自检 13/14、`syscalls: 81`、`nic_acceptance.py` 两网卡 PASS、无 PANIC；
  GUI 外观门禁矩形外 **0 像素**。

### Limits
- ISO9660 只读、仅主卷（Joliet/Rock Ridge/multi-extent/UDF 未做）、大写 8.3 名、单卷单挂；
  ATAPI 仅 PIO、单条 READ(10) ≤8 块、无换盘检测。
- AC'97 单次 ≤2.0 s、原生 48 kHz、纯轮询、不录音；MMIO BAR 分支未真机验证。
- FAT32 长名写仍限同目录 rename；非 ASCII 名终端显示乱码（无 CJK 字体，下一批做）。
- About 窗口 "14 upstream hobby OS projects" 属冻结外观文案，未改；真实上游数（16）见 README。

## [v0.15.0-alpha]

**两块从素材库移植的真实能力：Intel e1000 网卡（VMware 下第一次有网络）+ FAT32 VFAT 长文件名
读取；启动方式与 GUI 外观未变（有门禁脚本证明）。**

> 版本号从 v0.11.5 跳到 v0.15.0：`v0.12.0/0.13.0/0.14.0-alpha` 三个 tag 指向上一轮被判
> "外观被改动"且从未合入 main 的 `wip/all-ports` 提交；按发布纪律旧 tag 不移动不删除，
> 因此新版本使用未占用的 0.15.0，与旧 tag 共存。

### Added
- **Intel e1000 (8254x) 网卡驱动**（`kernel/drivers/e1000.c` 354 行 + `e1000.h`）：
  移植自 **Cinux** `kernel/driver/net/e1000.cpp`（**MIT**）。VMware Workstation 只提供
  e1000 虚拟网卡，此前 VMware 下完全没有网络。
- **链路层分发**（`kernel/net/net.c`）：新增 `ll_send()` / `ll_poll_rx()`，IP 栈不再直连
  RTL8139；选择顺序为 **RTL8139 优先、e1000 回退**（VMware 只有 e1000）。
- **FAT32 VFAT 长文件名读侧**（`kernel/fs/fatfs.c` +243 行）：`readdir` 拼回长名（`ls` 可见），
  新增 `resolve_component()` 让 `open/stat/cat` 也能用长名打开（wip 分支只做了显示这一半）；
  校验和不匹配的链丢弃，删除/改名时清理链。
- **长名开机自检**：`[fat32] lfn self-test PASS (open=1 payload=1 readdir=1)`。
- **`scripts/nic_acceptance.py`**：分别用 `-device rtl8139` 与 `-device e1000` 启动，
  注入 `ping 10.0.2.2`，断言链路行与 ≥3 条回复；两块网卡各自 PASS。
- **`scripts/gui_appearance_check.py`**：外观门禁，只容忍文档化的动态数值矩形。
- **`e1000_diag()`**：netstat / 自检超时时输出 GPRC/RDH/RDT/描述符状态。
- **文档**：`docs/audit-unfinished.md`（未完成项审计：P0=7/P1=19/P2=30 + 21 处过期文档条目）、
  `docs/port-candidates.md`（50 个开源自制 OS 的移植候选与许可评估）、
  `docs/licensing-audit.md`（上游许可证实读核对表）。

### Fixed
- **wip 分支不可直接 merge**：它基于 v0.11.0，merge 会删掉 v0.11.1–v0.11.5 的
  fork/socket/signal/GUI 工作；本版在 v0.11.5 上重新实现。
- **e1000 出处许可写错**：头注释原写 "Cinux GPL-3.0"，实际 Cinux/LICENSE 为 **MIT**，已改正。

### Verification
- `py -3 scripts/nic_acceptance.py` → **overall PASS**：rtl8139 开机自检
  `[net] selftest ping 10.0.2.2 -> reply in 1178 us` + 交互 ping 3/3；
  e1000 `[net] e1000 found at 0xfebc0000` + 交互 ping 3/3。
- `py -3 scripts/verify_fat32_write.py build/fat32_ata.img build/fat32.img` → **17/17**。
- FAT32 `ops self-test PASS (3/3)`、模块自检 `13/14`（`moos_intr` 为既有项）、
  `syscalls: 81 registered`、无 PANIC、启动日志 266 行（基线同）。
- GUI 外观：`scripts/gui_appearance_check.py` → 动态矩形外差异 **0 像素**。

### Limits
- **e1000 开机自检 ping 在 QEMU 11.1 下超时**（`GPRC=0`，设备模型不投递），同一次启动里
  交互 ping 3/3 正常；wip 上一轮同样复现，非本版引入。验收以 `scripts/nic_acceptance.py` 为准。
- e1000 为轮询驱动；FAT32 长名只读，写侧仍是 8.3；VMware 真正上网仍需先 `dhcp`。
- **SpiritFoxOS 上游没有 LICENSE 文件**：历史网络/块设备模块出处依据不足，已登记
  `docs/licensing-audit.md` 作为待确认项（本版新增代码不再从其拷代码）。

## [v0.11.5-alpha]

**GUI 窗口可以拖动、缩放、关闭了**（界面外观不变）。

### Fixed
- **拖动跳跃**：`gui_drag_begin_move/resize` 用"排队的点击坐标"算抓取偏移，而
  `gui_handle_mouse()` 先刷新实时指针再排空点击队列，同帧内的位移不在点击记录里
  → 第一帧跳一大截（实测按在 x=221、指针在 x=100，窗口被甩 100px）。改为以**实时指针**
  为基准并钳进屏幕。
- **右键误触发**：`gui_click_at()` 完全忽略 button，PS/2 的 bit1（右键）会开始标题栏拖动、
  在红色 x 上还会**关闭窗口**。现在只认左键，并记录 `g_click_right`。
- **指针回跳**：`gui_pointer_moved()` 把可见指针倒回排队的点击位置，导致拖动手势拿到旧位置。
  已删除该回跳。

### Verification
- 真 QEMU 交互（QMP 注入鼠标 + 截图分析）：拖动 `[gui] drag: 'Task Manager' at 24,24`
  标题栏像素恰好 +120/+80、旧矩形为纯桌面渐变、全桌面 0 残留像素；缩放
  `[gui] resize: 'Task Manager' 330x260` 边框恰好 +100/+60、0 残留；关闭
  `[gui] window closed:` 后矩形变桌面渐变，开始菜单可重新打开。
- 外观：与 `docs/baseline/base_gui.png` 对比，317 px 差异在动态文字区内，另有 78 px
  在 2 个簇内且都证实是动态文本（Task Manager 的 PMM 数字、About 的版本号数字——
  基线是 v0.11.0 截图，当前是 v0.11.4）；默认画面新增像素 = 0。
- 回归：启动日志 265 行、`syscalls: 81 registered`、自检 13/14、FAT32 ops PASS、
  无 PANIC。

## [v0.11.4-alpha]

**真正的多进程**：fork（写时复制）+ wait4 回收 + getppid，ring-3 shell 也能 fork/wait。
启动呈现与 GUI 外观零变化。

### Added
- **进程表与协作式进程调度**（`kernel/syscall/process.c/.h`）：每个进程 =
  {userspace_t + 16 KiB PMM 内核栈 + 预构建 user_context_t}；切换是
  push rbx/rbp/r12-r15 -> swap rsp -> pop -> ret，再切 CR3 与 `%gs:8`。
  子进程以 RUNNABLE 状态挂在一个手工构造的帧上，`ret` 落到 `proc_child_start` -> iretq
  且 `rax=0`。
- **fork + COW**（`userspace.c`）：PTE 软件位 9 = `PTE_COW`，父子共享同一批物理页并
  置只读；缺页时按 160 项"物理帧 -> 引用计数"表决定"复制"还是"仅恢复写权限"。
  CPL=3 与 CPL=0 两种缺页都先走 COW 再考虑信号。
- **wait4（61）/ getppid（110）/ getpid** 真实现：子进程退出成僵尸，父进程 wait4 拿
  `status = sig<<8 | code` 并回收槽位、地址空间与 fd；无子进程返回 `-ECHILD`，
  `WNOHANG` 支持。fd 表与 signal 槽位随 fork 克隆。
- **forktest**（`user/apps/forktest.c`）ring-3 验收程序；ring-3 shell 新增 `fork` 命令。
  另注册 clone(56)/vfork(58)/setpgid(109)/getpgrp(111)/getsid(124) -> **81 个系统调用**。

### Fixed
- **子进程经 int 0x80 退出会把 GS MSR 对留在"用户侧"**（该门不做 swapgs），父进程恢复后
  的 swapgs 反转了这对 MSR，下一次 syscall 从线性地址 8 取内核栈（RSP=0xf000ff53f000e2c3）
  导致整机停止打印；现在每次进程切换都把 GS_BASE/KERNEL_GS_BASE 归一化到内核侧。
- 删除 mmu.c 里从静态 MMU 池复制且无引用计数的 COW 辅助函数（双重释放风险）。

### Verification
- 干净重建 build=0、ISO 生成；启动日志 **265 行 == 基线**、`syscalls: 81 registered`、
  自检 13/14、`ring3 9/9`、`userspace 9/9`、FAT32 写+ops PASS、无 PANIC。
- `run forktest`：父 pid=1/子 pid=2、子写 'C' 后父仍读到 'Q'（COW 成立）、
  `wait4 -> pid=2 status=0x2a (code=42)`、无子进程时 `-ECHILD`、第二个子进程 status 0x7，
  `RESULT PASS`，退出 0；ring-3 shell `fork` 同样 PASS；`run sigtest` 回归 PASS。

### Limits
- 协作式调度（fork 返回 / wait4 阻塞 / yield / exit 才是切换点），无抢占；自旋子进程会
  饿死兄弟。
- 一次只有一个会话，root 退出会销毁存活子进程；fork 只复制 16 页用户窗口（mmap/brk 区
  不继承）；clone(CLONE_VM)/线程返回 `-EINVAL`，vfork == fork。
- wait4 status 编码是 `sig<<8|code`（不是 Linux 的 `(code&0xff)<<8`）。

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
