## 🚀 本次更新 (v0.30.0 → v0.31.0)

**桌面任务栏增强 + 快捷键（用户 2026-11-03 明确要求的界面更新）**：任务栏现在有**每个窗口一个按钮**
（点击=置顶/聚焦，最小化窗口也能点回来）、**实时时钟（CMOS RTC）**、**当前用户显示**；
按 **Windows/Super 键打开开始菜单**，ESC 先关菜单而不是退出桌面。安装盘与多用户是同一目标下的
后续批次，正在并行开发。

> ⚠️ 外观基线说明：用户本轮明确要求增强 GUI 任务栏，因此**外观基线按用户要求更新**
> （`docs/baseline/base_gui.png` 重新生成）。冻结规则仍然有效——风格、配色、字体、窗口样式全部保持
> SutureOS 原有自绘，没有搬任何其他 OS 的 UI。

---

### 📦 新增：任务栏能力

| 项目 | 内容 |
|---|---|
| 窗口按钮 | 每个可见窗口一个按钮（按打开顺序排列），活跃窗口高亮；点击 = `gui_set_active()` 置顶聚焦并取消最小化；最小化窗口额外画一条暗色条 |
| 时钟 | 从 CMOS RTC 读时间（`rtc_now_epoch()` + `rtc_format_epoch()`），显示 `HH:MM` |
| 当前用户 | `cred_euid()` → `userdb_by_uid()` 取用户名显示在时钟与 FPS 之间（多用户切换后自动变） |
| 快捷键 | **Windows/Super 键（E0 5B / E0 5C）** 切换开始菜单；**ESC** 在菜单打开时只关菜单（不再误退出桌面）；Tab 循环焦点、q 退出桌面（旧的全局键保留，在文本输入窗口聚焦时不抢） |
| 实现 | `kernel/gui/gui.c`：`gui_taskbar_windows()`/`gui_taskbar_btn_rect()`/`gui_current_user_name()`，任务栏绘制与任务栏点击命中；`gui_handle_keys()` 里处理 E0 前缀的 Win 键（**只认 make code，break code 会被忽略**——否则按一下会开关两次） |

### ✅ 验收（真实 QEMU + QMP 注入 + 像素）

| 项目 | 结果 |
|---|---|
| 任务栏内容 | 三个窗口按钮（Task Manager / Calculator / About SutureOS）+ `HH:MM` + `root` + `FPS n` 实际渲染（截图 6 种颜色，不是一整块纯色） |
| Win 键 | `sendkey meta_l` → 串口 `[gui] Win key: start menu opened`；再按/ESC → `[gui] ESC: start menu closed`；两次截图任务栏带像素差 **2546 px**（菜单真的弹出） |
| 任务栏按钮 | 点击 Calculator 按钮 → `[gui] taskbar: focus 'Calculator'`，窗口置顶 |
| 脚本 | `scripts/taskbar_acceptance.py` → **7/7 PASS** |
| 外观门禁 | 基线更新后 `gui_appearance_check.py` **动态矩形外 0 像素**（时钟/用户名/FPS/按钮高亮已登记为 live 区域） |
| GUI 回归 | 图片查看器 PASS、GUI 中文 4/4、文本编辑器 5/5（开始菜单坐标未变） |

### ⚠️ 已知限制（如实记录）

1. 任务栏按钮最多显示 6 个（`TASKBAR_MAX_BTNS`），再多的窗口不显示按钮（仍可用 Tab 切换）。
2. 按钮标题截断到 14 字符；按钮区右侧预留 24 字符给时钟/用户/FPS，窗口标题很长时按钮会提前停止绘制。
3. 没有任务栏右键菜单/固定应用/托盘；最小化只能点标题栏按钮，没有"显示桌面"。
4. Win 键只在 **GUI 输入循环**里处理（QEMU `sendkey meta_l` 已验证；真实键盘的 Win 键走同一条 E0 5B 路径）。
5. 时钟只有 `HH:MM`、无秒/无时区选择（RTC 为本地 CMOS 时间）。

### 📜 出处与许可（本版）

- 任务栏增强是 SutureOS 自有实现（GPL-3.0），复用已有的 `gui_*` 绘图 API、RTC 驱动与 userdb，没有移植外部 UI 代码。

### 📦 下载

- 附件 `sutureos.iso`（15673344 字节）
- sha256：`965a5d3dd40feb2141e7cdb7101bcbd9f9a34c23d3c818884768433d1f6c038f`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.31.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.31.0-alpha
