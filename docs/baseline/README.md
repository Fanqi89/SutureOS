# 外观基线（Appearance baseline）

三张截图共同构成 SutureOS 的“外观冻结”基准：

- `base_terminal.png` —— 开机完成后进入的终端画面（滚动英文日志 → banner →
  Welcome to SutureOS Terminal / sutureos#）。**启动方式不变**。
- `base_gui.png` —— **v0.59.0 起的新桌面**：在终端输入 `startgui` 进入的 Rust 桌面外壳
  （圆角窗口 + 轻亚克力 + 任务栏 + 开始菜单 + 伪 3D 应用图标）。用户 2026-11-21 明确批准
  用新外壳替换 C 桌面，并把视觉方向定为“圆角 + 轻亚克力 + Fluent 底 + 伪 3D 图标”。
- `base_gui_classic.png` —— 原 C 桌面（任务栏、Task Manager / Calculator / About 三窗口），
  现在通过 `startgui-classic` 进入，保留它自己的外观门禁，防止新旧两条路径互相踩坏。

## 规矩

1. **启动方式不变**：滚动英文日志 → 系统名 banner → 黑底白字终端，GUI 只能在终端显式输入
   `startgui` 进入；开机呈现不得被改动。
2. **桌面外观以对应基线为准**：新桌面 = `base_gui.png`，C 桌面 = `base_gui_classic.png`。
   门禁 `scripts/gui_appearance_check.py --profile new|classic` 只允许实时数据
   （RTC 时钟、Task Manager 计数器、焦点高亮等）不同，其余区域必须逐像素相同。
3. **视觉方向**（用户 2026-11-21 批准）：圆角 + 轻亚克力（只用于背景和浮层）+ 微透叠层 +
   Win11 Fluent 为底 + 拟物化伪 3D 应用图标 + 轻微空间感；不得把别人的桌面环境整层搬进来。
4. **每次改动后的验收**：
   - 新桌面跑 `scripts/gui_baseline_shot.py`，C 桌面跑
     `scripts/gui_baseline_shot_classic.py`，两者都必须 **0 像素越界**；
   - 启动终端截图与 `base_terminal.png` 逐像素相同（版本号变化需显式说明）；
   - 启动日志逐行对比只允许内核地址、PMM 空闲页数、RTC 时间、版本号字符串不同；
   - 开机自检类输出只在失败时打印，不往启动日志里加行。
