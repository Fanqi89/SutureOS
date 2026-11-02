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
| B2 | v0.17.0-alpha | **ramfs 容量提升**（64 节点/2 KiB → 动态） | 自研（参考 tmpfs 思路） | 写 100 个文件 + 单文件 64 KiB 成功，`/etc` 种子不截断 | 下一轮 |
| B3 | v0.18.0-alpha | **ext2 写支持**（分配器/间接块/目录项） | XJ380 `ext4.cpp`(Apache-2.0)、Deshab `ext4.c`(Apache-2.0) 算法参考 | 挂载后 mkdir/write/rename/unlink，宿主 `e2fsck`/独立解析校验一致 | 下一轮 |
| B3 | v0.18.0-alpha | **NVMe/virtio/AHCI flush 回归**（已有，补验收） | Cinux(MIT) | 断电模拟：write 后不 flush 丢数据、flush 后可见 | 下一轮 |
| B4 | v0.19.0-alpha | **virtio-gpu 2D**（QEMU 显示加速，framebuffer 回退） | Deshab `virtio_gpu.c`(Apache-2.0) | 检测到设备时走 GPU 呈现、否则回退；外观逐像素不变 | 下一轮 |
| B4 | v0.19.0-alpha | **图片查看器**（BMP/QOI 解码，SutureOS 自绘 UI） | HBOS `bmp.c`(GPL-3.0)、PlantOS `qoi.c`(MIT) | GUI 里打开测试图，像素/尺寸正确；外观门禁通过 | 下一轮 |
| B5 | v0.20.0-alpha | **ring-3 文件工具**（ls/cat/cp/mv/rm/mkdir） | 自写（VimtuOS busybox 仅参考，GPL-3.0） | `ushell` 里跑通，宿主侧比对文件系统 | 下一轮 |
| B5 | v0.20.0-alpha | **HTTP 下载客户端**（ring-3 `wget`） | HBOS `wget.c`(GPL-3.0)、Deshab `curl`(Apache-2.0) 参考 | 从 slirp 内建 HTTP 服务下载文件并校验 sha256 | 下一轮 |
| B6 | v0.21.0-alpha | **CJK 字体 + UTF-8**（终端优先，GUI 后续） | 素材库内自带 CJK 的仓库（实读确认许可）或 Unifont(公共域) | 终端打印中文不出现空白/替换符；ASCII 外观不变 | 下一轮 |
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
