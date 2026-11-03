# SutureOS 移植路线图（自研操作系统素材库 → SutureOS）

目标：把 `C:\Users\fanqi\Desktop\自研操作系统`（49 个文件夹 / 50 个仓库）里**可移植且有优点**的
功能与模块，按许可证合规地移植进 SutureOS。约束与做法：

- **外观冻结**：启动呈现（滚动英文日志 → 系统名 banner → 黑底白字终端，GUI 只能 `startgui` 进入）
  与 GUI 外观（任务栏/窗口边框/标题栏/配色/字体/指针/开始菜单）不得改变；移植只借功能，
  界面必须由 SutureOS 自己绘制（`kernel/gui/apps.c` 风格）。
- **许可证**：SutureOS = GPL-3.0。可直融 MIT / Apache-2.0 / BSD / Unlicense / MPL-2.0 / GPL-3.0；
  **禁用** GPL-2.0-only（DragonOS、Utopia、Reedx-Kernel、hudos）与 AGPL-3.0（Nilix、UniOS）；
  **无 LICENSE 的仓库**（SpiritFoxOS、KunOS、QNOS、C-UnknownOS、GrapeOS-Course、aether-naos）
  只能看算法，不能拷代码。逐条实读结果见 `docs/licensing-audit.md`。
- **每一批**都必须：真实 QEMU/宿主验收 + 外观门禁（`scripts/gui_appearance_check.py`）+
  发布纪律（提交 main、新附注标签、GitHub Release + ISO、旧标签不动、README/CHANGELOG 更新）。

## 批次计划（每批一个版本，独立验收）

| 批次 | 版本 | 内容 | 来源（许可） | 验收判据 | 状态 |
|---|---|---|---|---|---|
| B1 | v0.16.0-alpha | **ATAPI 光驱 + ISO9660 只读** | Uinxed-Kernel `atapi.c`(Apache-2.0)、CoolPotOS `iso9660`(MIT) | `ls /mnt/cd`/`cat` 内容与宿主一致、开机 selftest PASS、无光驱时安静跳过 | ✅ 本轮完成 |
| B1 | v0.16.0-alpha | **AC'97 声卡 PCM 输出** | HBOS `src/ac97.c`(GPL-3.0) | `-audiodev wav` 落盘：时长 1.00±0.05 s、峰值 ≥0.1 FS、过零率 440±22 Hz | ✅ 本轮完成 |
| B2 | v0.16.0-alpha（提前到 B1） | **FAT32 长文件名写侧**（创建/改名/删除长名） | ChaN FatFs 算法（PlantOS MIT 血统） | 客机 create/readdir/rename/unlink/utf8 自检 PASS + 宿主 21/21（无孤儿链、无残留、UTF-8 解码回原串） | ✅ 本轮完成 |
| B2 | v0.17.0-alpha | **ramfs 容量提升**（64 节点/2 KiB → 512 节点/256 KiB，堆上 4 KiB 块） | 自研 | `[ramfs] capacity self-test: big=65536 bytes chunks=16 files=100 nodes=104/512` + `[selftest][xj_vfs] PASS` + shell `df`/`mkdir`/`touch` | ✅ 本轮完成 |
| B3 | v0.17.0-alpha | **ext2 写支持**（位图分配/直接+单+双间接/目录项/rename/truncate） | XJ380、Deshab-OS ext4（Apache-2.0 算法参考） | 1 KiB/4 KiB 写自检 PASS；宿主 `verify_ext2_write.py` **44/44 ×2**（276 块双间接产物逐字节）；只读回归 47/47、35/35、16/16 | ✅ 本轮完成 |
| B3 | — | **NVMe/virtio/AHCI flush 回归**（代码已有，补验收） | Cinux(MIT) | **说明：审计里"无 flush"已过期**——`nvme.c`/`virtio_blk.c`/`ahci.c` 都已实现 `->flush` 并挂到 `blk_flush()`，FAT32/ext2 `sync` 会调用；断电一致性未做破坏性测试 | ⏭ 降级为 P2 |
| B4 | v0.19.0-alpha | **virtio-gpu 2D**（QEMU 显示加速，framebuffer 回退） | Deshab `virtio_gpu.c`(Apache-2.0) | 检测到设备时走 GPU 呈现、否则回退；外观逐像素不变 | 下一轮 |
| B4 | v0.18.0-alpha | **图片查看器**（BMP/QOI，SutureOS 自绘界面、懒启动） | HBOS `src/bmp.c`(GPL-3.0)、PlantOS `src/plic/qoi.c`(MIT) | 真实鼠标点开始菜单 → 窗口创建 → 截图四象限像素精确匹配（BMP + QOI 各一轮）；默认桌面仍 3 窗口 | ✅ 本轮完成 |
| B5 | v0.18.0-alpha | **ring-3 文件工具**（rls/rcat/rcp/rmv/rrm/rmkdir/rtouch/rdf + ushell fork/execve/wait4；内核补目录 fd、exec argv 帧、fd 从 3 起） | 自研（argv/目录 fd 约定参考 VimtuOS GPL-3.0） | ushell 22 条命令自断言 37/37 + 宿主 FAT32 21/21 + 无 PANIC | ✅ 本轮完成 |
| B5 | v0.18.0-alpha | **ring-3 文件工具**（rls/rcat/rcp/rmv/rrm/rmkdir/rtouch/rdf + ushell fork/execve/wait4；内核补目录 fd、exec argv 帧、fd 从 3 起） | 自研（argv/目录 fd 约定参考 VimtuOS GPL-3.0） | ushell 22 条命令自断言 37/37 + 宿主 FAT32 21/21 + 无 PANIC | ✅ 本轮完成 |
| B6 | v0.19.0-alpha（部分，先做了终端写文件） | **终端文本工具** write/append/hexdump | 自研（GPL-3.0） | 宿主独立解析 FAT32 镜像逐字节一致 + 8/8 断言 | ✅ 本轮完成 |
| B6b | v0.20.0-alpha | **终端 CJK 中文显示**（GB2312 16×16 点阵、UTF-8 流式解码、双宽排布、滚动安全覆盖层） | ZhengGeDianHei-16.ttf（素材库 HBOS，**SIL OFL-1.1**，构建期渲染） | 宿主截图按 16×16 逐像素匹配 4 字形 + 滚动后复验 **10/10 PASS** | ✅ 本轮完成 |
| B7 | v0.22.0-alpha | **USB EHCI + HID 键鼠** | Uinxed-Kernel `usb/host/ehci`(Apache-2.0) | QEMU `-device usb-ehci -device usb-kbd/usb-mouse` 下键盘/鼠标可用 | 下一轮 |
| B8 | v0.23.0-alpha | **USB Mass Storage（U 盘）** | Uinxed-Kernel `usb/storage`(Apache-2.0) | `-device usb-storage` 下挂 FAT32 U 盘、写自检 17/17 | 下一轮 |
| B9 | — | **HDA 声卡**（比 AC97 兼容更好，AC97 完成后再评估） | VimtuOS `hda64.cpp`(GPL-3.0) | 与 AC97 同一套 WAV 判据 | 可选 |
| B10 | — | **文本编辑器**（GUI 自绘 UI） | VimtuOS `apps/edit/edit.c`(GPL-3.0) 参考 | 能打开/编辑/保存文件；外观门禁通过 | 可选（红线敏感） |

## 明确不做 / 只做算法参考

| 项目 | 原因 |
|---|---|
| DragonOS / Utopia / Reedx-Kernel / hudos 的任何代码 | GPL-2.0-only，与 GPL-3.0 不兼容 |
| Nilix / UniOS | AGPL-3.0，网络使用义务超出本项目分发模型 |
| SpiritFoxOS 的任何**新**代码拷贝 | 仓库无 LICENSE 文件；历史拷贝登记在 `docs/licensing-audit.md` 待确认 |
| 替换 GUI 框架 / 合成器 / 窗口管理器 / 桌面皮肤 | 违反外观冻结红线 |
| 搬用其它系统的窗口控件/布局/图标 | 同上，只借功能 |

## 每批的固定动作（验收清单）

1. 代码：新增文件带「来源项目 + 来源路径 + 实际许可证 + 改动摘要」头注释。
2. 构建：`py -3 scripts/build.py` + `py -3 scripts/build_iso.py`，零新增 warning。
3. 启动：`scripts/qemu_boot.ps1` 跑一次，日志无 PANIC、自检数不减、无新增 FAIL。
4. 功能：按批次判据跑真实 QEMU + 宿主侧脚本，数字写进 release notes。
5. 外观：`scripts/gui_appearance_check.py build/shot_look.ppm` → 动态矩形外 0 像素。
6. 发布：改 `include/sutureos/version.h` → CHANGELOG + release notes + tag notes →
   `scripts/publish_all.ps1` 一条命令（提交/标签/Release/ISO/自证）。

| B6c | v0.21.0-alpha | **GUI 中文文本**（UTF-8 + 16×16 点阵，复用终端字体子集） | 同一 OFL-1.1 字体子集 | 图片查看器 `缩放 xN` 字形在截图中逐像素匹配 4/4 | ✅ 本轮完成 |
| B5b | v0.22.0-alpha | **wget（HTTP/1.0 GET over 内核 TCP）** | SutureOS 自有（GPL-3.0） | 宿主 HTTP 服务器 + 关 QEMU 后解析 FAT32 镜像逐字节比对 **8/8** | ✅ 本轮完成 |
| B8 | v0.23.0-alpha | **USB 2.0 EHCI + 大容量存储（BOT/SCSI）** | HanOS（MIT）骨架 + HBOS（GPL-3.0）BOT 参考 | QEMU 有/无 USB 两组 + `verify_usb_disk.py` 8/8（只读整盘 CRC 未变 5/5） | ✅ 本轮完成 |
| B8b | v0.24.0-alpha | **FAT32 多卷**（多挂载点同时可用，去掉单卷限制） | SutureOS 自有（GPL-3.0） | 双卷同挂 + 交叉写 + 宿主双镜像逐字节校验 **12/12** | ✅ 本轮完成 |
| B10 | v0.25.0-alpha | **文本编辑器**（GUI 第五个应用：加载/编辑/回车保存） | SutureOS 自绘（GPL-3.0） | 真实鼠标+键盘注入 → 宿主解析 FAT32 镜像逐字节 **5/5** | ✅ 本轮完成 |
| B11 | v0.26.0-alpha | **命令行文本工具**（wc/head/tail/grep/stat） | SutureOS 自有（GPL-3.0） | shell 造多行文件 + 五命令断言 + 宿主 FAT32 逐字节 **7/7** | ✅ 本轮完成 |
| B9 | v0.27.0-alpha | **Intel HDA 音频**（控制器 + codec 枚举 + BDL 播放） | PlantOS（MIT）hda.c 骨架 | 宿主 WAV 校验 48000 Hz/2ch/16bit/1.002 s/0.5000 FS/440.01 Hz **6/6** | ✅ 本轮完成 |
| B12 | v0.28.0-alpha（部分） | **`sync` 命令**（多卷挂载表遍历 flush） | SutureOS 自有（GPL-3.0） | 不 umount + 宿主 FAT32 逐字节 **6/6** | ✅ 本轮完成 |
| B13 | v0.29.0-alpha | **EXT2 多卷**（双卷同挂 + 跨卷写入根因修复） | SutureOS 自有 | 卷 1 写验证器 40/40、卷 2 fixture 35/35、卷 2 变化 48B、卷 1 无卷 2 文本 → **12/12** | ✅ 本轮完成 |
| B14 | v0.30.0-alpha | **squashfs 多卷** | SutureOS 自有 | 双 squashfs 同挂 + 各自 ls/cat + 宿主两镜像 83/83 → **7/7** | ✅ 本轮完成 |

## 结项时明确"未做"的两项（不是遗漏，是评估后推迟）

| 编号 | 状态 | 内容 | 为什么推迟 |
|---|---|---|---|
| B15 | ⏳ 未做 | **USB 热插拔重扫 + EHCI HID 键盘** | 需要端口变化检测与整套重新枚举/重注册流程；v0.23 的存储栈是单次开机枚举。QEMU 侧还要用 `-blockdev node-name=` 才能 `device_del`/`device_add`（用 `-drive if=none` 会连后端一起销毁）。PS/2 键盘已经可用，HID 属于"换一条输入路径"，优先级低于已有能力。 |
| B16 | ⏳ 未做 | **virtio-gpu 2D 呈现增强**（显存脏矩形/多缓冲） | 现有 framebuffer 路径在 QEMU 的 `-vga std` 下已稳定且外观门禁逐像素通过；引入 virtio-gpu 需要新的 PCI 驱动 + 扫描回写路径，收益主要是性能，风险是可能改变启动呈现。按"外观冻结"红线评估后推迟。 |

两项都保留了调研结论与参考出处（见 `docs/provenance.md` 的 collection 清单），下一轮可直接接着做。
| B17 | v0.31.0-alpha | **GUI 任务栏增强 + Windows 键快捷键**（窗口按钮/时钟/当前用户） | SutureOS 自绘（用户 2026-11-03 要求，外观基线已更新） | 真 QMP 注入 + 截图断言 **7/7** | ✅ 本轮完成 |
| B18 | v0.33.0-alpha | **ISO 系统安装盘**（真实写盘/分区/Limine 引导/FAT32 格式化/系统文件//etc 播种） | SutureOS 自有 + Limine 布局 | 三阶段宿主验收 **56/56**（含无 ISO 从硬盘启动 + su alice + 空密码路径） | ✅ 本轮完成 |
| B19 | v0.32.0-alpha | **多用户**（useradd/userdel/users/passwd/login/su + GUI Switch User） | SutureOS 自有（GPL-3.0） | 多用户 54/54 + GUI 切换 13/13 + 宿主 PBKDF2 重算校验 | ✅ 本轮完成 |