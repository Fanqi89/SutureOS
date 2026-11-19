## 🚀 本次更新 (v0.47.0 → v0.48.0)

**动效开了个头，时钟修对了**：从开始菜单打开窗口时，新窗口会用 **3 帧淡入**（0x58→0x2c→0x12 黑色叠层）
柔和地出现；任务栏时钟从"UTC 只读一次"改成**本地时间（UTC+08:00）并每分钟刷新**。外壳验收扩到 **37/37**。

---

### ✨ 窗口打开淡入（v0.48.0）

| 项 | 做法 |
|---|---|
| 状态 | `Shell::intro_frames / intro_idx`，`launch_window()` 置 3 帧 |
| 绘制 | 窗口画完后对窗口矩形叠一层黑色 `rect_fill`，alpha 依次 0x58 / 0x2c / 0x12 |
| 空闲跳过 | 淡入期间**不跳过**空闲帧（否则动画会卡在第一帧） |
| 开销 | 每帧一次矩形填充（约 17 ms），仅 3 帧 |
| 日志 | `[gui-shell] intro fade 3 2` / `2 2` / `1 2` |

### 🕒 本地时钟（v0.48.0）

| 项 | 做法 |
|---|---|
| 时区 | RTC 是 UTC，固定 **+08:00** 偏移（SutureOS 还没有时区数据库，先写死并在日志里标出来） |
| 刷新 | 每 6000 tick（≈1 分钟）重算一次，日志 `[gui-shell] clock update local=HH:MM` |
| 启动日志 | `[gui-shell] clock utc=12:24 local=20:24 (+08:00)` |
| 验收 | 新增 2 项：日志存在 + **local == UTC+8 关系成立**（脚本真的去算了一遍） |

### ✅ 验收

| 项目 | 结果 |
|---|---|
| 外壳 `slint_gui_shell_acceptance.py` | **37/37 PASS**（原 35 项 + 本地时钟 + 淡入） |
| 时钟 | `clock utc=12:24 local=20:24 (+08:00)`；`clock update local=20:25`、`20:26` |
| 淡入 | `intro fade 3/2/1` 三行都在 |
| 回归 | 静态像素 **21/21**、交互 **13/13**、C 桌面 `apps_taskbar` **9/9**、GUI 外观门禁 **0 像素**、内核 13/14 |
| 性能不回退 | `profile/30: full=30 hit=27 miss=27 win=213 bar=27 ticks`（v0.47 是 280） |

截图：`docs/screenshots/shell-prototype-v0.48.0.png`。

### ⚠️ 已知限制（如实记录）

1. 淡入只有"打开窗口"这一条路径：关闭、最小化、最大化/还原**还没有**过渡动画。
2. 淡入是整块矩形压暗（不是真正的透明度动画），3 帧在 ~13 fps 下约 230 ms。
3. 时区写死 +08:00，没有 TZ 数据库/夏令时；分钟刷新依赖 tick 累积（QEMU 下 tick 可能偏慢）。
4. 圆角覆盖率仍是整数像素中心查表（1 px 级 AA）；默认两窗口重叠处不缓存。
5. 拖动窗口仍会重建窗口缓存（几何每帧变），图层合成器（B35）尚未开始。

### 📜 出处与许可（本版）

- 全部为 SutureOS 自有实现（GPL-3.0），未拷贝第三方代码。

### 📦 下载

- 附件 `sutureos.iso`（17496064 字节）
- sha256：`396fbcb1122715e17cca117d9c965b740c2a68f74c518839c89526953796abd8`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.48.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.48.0-alpha
