# SutureOS v0.59.0-alpha

**桌面换代：`startgui` 现在启动 Rust 桌面外壳（圆角 + 轻亚克力 + Fluent + 伪 3D 图标），
原 C 桌面保留为 `startgui-classic`；同时落地最小 Wayland 服务端（named AF_UNIX +
wl_compositor/wl_shm/wl_seat 子集 + ring-3 wlclient + `wlshow` 上屏）。**

本版在 v0.58.0-alpha（新 GUI 第三轮：alpha 阴影、窗口内键盘焦点、4 帧几何过渡）的基础上，
按用户 2026-11-21 的明确授权完成两件大事：①用新外壳替换 C 桌面成为默认桌面，并把视觉方向
定为“圆角 + 轻亚克力 + Win11 Fluent 底 + 拟物化伪 3D 图标 + 轻微空间感”；②把 Wayland 从
“四个内核前置”推进到“最小服务端 + ring-3 客户端共享内存 surface 真正上屏”。
启动方式与启动呈现完全未变：滚动英文日志 → 系统名 banner → 黑底白字终端 → 显式输入 `startgui`。

---

## ■ 关键修复

| 现象 | 根因 | 修复 |
|---|---|---|
| 每扇窗周围一圈纯黑硬边（实测像素 (0,0,0)） | v0.46 起窗口层缓存把阴影烘进**零初始化（黑）**缓存再整块拷贝，阴影没有与场景合成 | 层缓存只保留不透明窗体；阴影改为按距离查表的 8 层 1px alpha 环逐帧合成，圆角 AA 用所在位置壁纸预填；验收断言阴影带 == 壁纸×(1-α) 且重叠窗口阴影正确压暗下层 |
| 任务栏/开始菜单/关机动画里每个 Logo 都垫着一块白方块 | `scripts/mklogo_assets.py` 的 `load()` 先 `convert('RGB')` 丢掉 alpha，透明背景的白色 RGB 被原样画到屏上 | 生成 `0xAARRGGBB` 并在 `blit_logo_region` 里 alpha 混合；验收断言不透明像素与 PNG 完全一致、透明像素 0 个纯白、透明区均值跟随背景 |
| 快速连打丢字（实测 "hello" → "hllo"） | 外壳一帧从 8042 排空多个字节，但只能向 Rust 侧传 1 个 key/char，旧代码只保留最后一个 | `slint_bridge.c` 加 32 项按键队列，逐帧喂一个 key/char |
| ring-3 Wayland 客户端读不到任何回包 | `fd_pipe_end()` 只认 pipe fd 与小描述符表，编码 unix fd（≥200）不被 read/write/close 路由 | 加 unix fd 分支，并在 read/write 路径挂 `wl_sock_pump_fd()` |
| 从 `MAP_SHARED` 映射写文件被判 -EPERM | `el0_ptr_ok()` 只认启动时 grant 的窗口，mmap/brk 出来的用户页在窗口之外 | 加“页表兜底”：逐页 `mmu_walk(...,0)` 要求 PTE_P\|PTE_U（内核页 U=0 仍被拒） |
| 250 KiB WL.BIN 只写一半（151552/256000） | 不是 flaky：TCG 下 FAT32 写约 20 KB/s，验收 8 s 窗口截断写入 | 验收等待提到 35 s，整份写完；宿主逐字节复核 256000 B |
| 外壳验收偶发 54/55（`restore transition completes`） | 后一组双击在 4 帧过渡中途把 restore 打断（TCG 时序） | 断言改为“完成，或被打断但已跑≥2 帧”，并在测试里把 restore 点击与第一次点击拉开 >1.5 s |

## ■ 新增能力

**桌面换代（用户授权）**
- `startgui` = Rust 桌面外壳；原 C 桌面（六应用 + 任务栏）保留为 **`startgui-classic`**，
  `startgui test` 仍走它的指针自检；Rust 对象未链接时如实回退并打日志。
- 外观基线拆两套：新桌面 `docs/baseline/base_gui.png`、C 桌面 `base_gui_classic.png`；
  `scripts/gui_appearance_check.py --profile new|classic`；新增 `scripts/desktop_acceptance.py`
  与 `scripts/gui_baseline_shot_classic.py`。

**视觉方向落地**
- 拟物化伪 3D 应用图标：开始菜单 22px、窗口标题栏 14px；纵向渐变盘 + 顶部高光 + 底部暗边 + 投影，
  四个应用各自 accent（蓝/青/橙/粉），纯整数逐行 inset 画圆角（避开软浮点 sqrt）；标题栏加 accent 下划线。
- 每窗真实应用内容（新模块 `rust/suture-gui/src/apps.rs`）：系统设置三行设置项、
  任务管理器实时 uptime/CPU 百分比 + 两条进度条、图片查看器棋盘格 + 真实 48px Logo 3× + 文件名标注、
  文本编辑器三行带行号代码 + 光标块。

**最小 Wayland 服务端**
- `kernel/ipc/wl_sock.c`：AF_UNIX **named endpoint**（`bind`/`connect`，int 0x80 号 15/16，懒初始化，
  无需改 kmain）；`kernel/ipc/wl_server.c`：wl_display / wl_registry / wl_callback / wl_compositor /
  wl_surface / wl_shm / wl_shm_pool / wl_buffer / wl_seat 子集（Wayland 线格式），
  commit 时从客户端经 SCM_RIGHTS 交付的 memfd 读像素、算 FNV-1a、保留 320×200 像素拷贝。
- `user/apps/wlclient.c`：真 ring-3 客户端（`run wlclient`）——registry roundtrip、bind 三个 global、
  建 memfd + MAP_SHARED、create_pool/create_buffer/attach/commit、等 buffer.release、
  把同样像素写到 `/mnt/fat/WL.BIN`。
- 新增 **`wlshow`**（`kernel/gui/wl_view.c`）：把最后 commit 的 wl_surface 整屏呈现
  （整数最近邻缩放 + accent 边框），ESC 返回终端。

**验收脚本**
- 新增 `desktop_acceptance`、`gui_apps_content_acceptance`、`wl_server_acceptance`、`wl_show_acceptance`、
  `gui_baseline_shot_classic`；`logo_acceptance` 改为 alpha 感知（不透明像素逐像素 + 透明区背景跟随）；
  六个 C 桌面验收脚本改走 `startgui-classic`（`-` 用 `sendkey minus`）。

## ■ 验证

同一份最终 ISO（sha256 见下）真跑，全部通过：

```
desktop_acceptance            19/19   （startgui 进新桌面、任务栏时钟、标题栏图标、菜单伪 3D 图标/各自 accent、
                                      ESC 回终端、startgui-classic 仍可进旧桌面并 q 退出）
slint_gui_shell_acceptance    55/55   （含关窗、拖动、最小化/最大化、过渡、键盘输入、空闲跳帧、缓存命中）
gui_apps_content_acceptance   25/25   （四窗内容互异、设置三行、实时 uptime/CPU 数字变化、Logo 3×、编辑器三行）
logo_acceptance               14/14   （24px/48px 不透明像素与 PNG 一致、透明区 0 纯白、F10 关机动画 + ACPI S5）
wl_server_acceptance          28/28   （协议 trace + 客户端/服务端/宿主三方 hash c7763445 + 宿主逐字节 256000 B）
wl_show_acceptance             9/9    （surface 真的上屏：4160 个 accent 边框像素、中心 59%% 非黑）
slint_gui_acceptance          21/21   （静态原型 + 真亚克力模糊限定）
slint_gui_interactive         13/13
apps_taskbar_acceptance        9/9    （C 桌面经 startgui-classic）
gui_appearance_check new       0 像素越界
gui_appearance_check classic   0 像素越界
boot self-tests               16/16
symbol-scan                    1405 全局符号 / 0 冲突（143 对象）
```

产物：`kernel.elf` 6,400,672 B；`sutureos.iso` 23,410,688 B。

## ■ 产物（发布页附件）

- 文件名  sutureos.iso
- 字节数  23,410,688
- SHA-256: 944C6C519354276129C816F67DF3A151CD0700958B576D63AB290860BE5FCD1B
- 下载直链: https://github.com/Fanqi89/SutureOS/releases/download/v0.59.0-alpha/sutureos.iso
- 发布页: https://github.com/Fanqi89/SutureOS/releases/tag/v0.59.0-alpha

## ■ 已知限制

- 合成器仍**没有 vsync/交换链**（双缓冲 + 完整/局部呈现，无垂直同步源）；当前只有“渲染完成再呈现”。
- 最小 Wayland 服务端是**子集**：无 `wl_display.error/delete_id`、无 `wl_surface.frame` 回调、
  无 damage 追踪、无 scaling/subsurface/region 语义、除 XRGB8888 外格式不 blit；
  `wl_seat` 的 pointer/keyboard 是 inert 对象（没有输入事件路由）；单客户端（第二连接接管）；
  `scm_fd` 队列按方向而非按连接；服务端由客户端 syscall 驱动，客户端不读会丢事件（有 dropped 日志）。
- 每窗内容是**静态演示内容**：任务管理器 CPU 百分比是确定性合成值（shell 没有 CPU 占用计数器），
  uptime 才是真实 `shell_tick()`；文本编辑器是只读三行 + 光标块（可编辑行仍由外壳的键盘焦点行负责）。
- Wayland 像素缓存上限 320×200；`wlshow` 是整屏呈现而非嵌入桌面窗口（后续接合成器窗口）。
- C 桌面 `startgui-classic` 与它的应用验收仍在，但不再是默认桌面。
- 所有结论来自 QEMU（未做真机验证）。
