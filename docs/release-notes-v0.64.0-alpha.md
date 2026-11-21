# SutureOS v0.64.0-alpha

**四个桌面细节一次做完：菜单会滚动了、计算器会算小数了、按键归属有了唯一一张表、present 路径开始探测垂直回扫（并如实报告 QEMU 上没有可用 vblank）。**

这一版没有换外观：默认桌面（壁纸/任务栏/两个默认窗口）逐像素仍是基线，`startgui` 的行为、终端与启动呈现一个字都没动。所有改动都在「应用与外壳的交互层」和「present 路径的度量」上。

---

## ■ 关键修复（现象 → 根因 → 怎么修）

| 现象 | 根因 | 怎么修 |
|---|---|---|
| v0.63 计算器窗口里，4×4 键盘板被一条「输入: 键入文本 Tab 切换窗口」压住 | 「谁吃键」写在帧循环的 if 链里，「谁不该画输入行」写在 `draw_focus_line` 的硬编码跳过列表里 —— **同一份知识维护在两处**，加应用时漏改一处就会出现这种错位 | 抽出唯一的 `KEY_OWNERS` 注册表（slot + accent + name + sink）：帧循环查表分派、`draw_focus_line` 查表决定是否画输入行。启动打 `[gui-shell] key owners=4 mask=60`，门禁按它断言 shell 真在走这张表 |
| 开始菜单只能放 6 项 —— 第 7 项会把面板顶到屏幕外/盖住任务栏 | 面板高度写死成 `46 + 项数×44 + 14`，行数与高度耦合 | 改成「可视 6 行固定 324 px + 滚动」：可视窗口固定，选择自动滚进视野、Home/End 跳首末、右缘滚动条可点（上半上滚/下半下滚）；第 7 行做成**动作行**「退出到终端」（`MenuTarget` 枚举：窗口槽位 or 外壳动作） |
| 计算器只会整数、除零之外没有别的语义，鼠标点它只会把窗口聚焦 | 当初只做了 `i64` 四则 + 4×4 键盘板，没有小数点/百分比/内存的模型；窗口点击也没有「把点击映射到应用内容」的路径 | 重写为定点 1/10000（4 位小数、饱和、无 f32）、`%` 两种语义、MC/MR/M+/M-、±/DEL/CE/AC/00/√/x²/1÷x，5×6 键板；`apps.rs` 暴露 `calc_keypad_hit`/`calc_tap`/`calc_slot_center`/`calc_layout`，`shell.rs` 在窗口体点击分支里把屏幕坐标换成槽位再 `calc_tap()` |
| present 的「60 Hz 栅格」只是一个软件假设，没有任何硬件依据，也没有延迟直方图 | 桌面从来没有尝试过垂直回扫；`SwapState` 只有平均延迟和区间直方图 | 新驱动 `kernel/gui/vsync.c`（`0x3DA` bit3 上升沿 + HPET 计时 + CRTC 交叉验证 + 速率校验 40–120 Hz + 有界等待）；present 每次上屏前一次等待，记录**等待直方图**与**产出→上屏延迟直方图**。**实测 QEMU 的 0x3DA 是「每读翻转」的假信号，驱动诚实判不可用并降级**（见下） |

---

## ■ 新增

### 1. 「焦点窗口拥有按键」收成一张注册表

- `KEY_OWNERS`：图片查看器(2) / 文本编辑器(3) / Wayland 视图(4，不吃键但不给输入行) / 计算器(5)；
  普通窗口（系统设置、任务管理器）继续用一行输入带。
- 启动日志 `[gui-shell] key owners=4 mask=60`（bit = 窗口槽位）。
- 门禁 `scripts/gui_keyowners_acceptance.py` **13/13**：`input win=1 text=abc` / `input commit 1 3`、
  `editor key`、`image shift 1`、`calc key` 都各自到位，而四个拥有者窗口**一条 `input win=N` 都没有**。

### 2. 开始菜单可滚动 + 第 7 行「退出到终端」

- 可视 6 行 = 324 px（面板上沿 y=398、行 0 从 y=444 —— 与 v0.62/v0.63 完全一致，既有验收坐标零改动）。
- ↑/↓ 选择自动滚动、Home/End 跳首末、滚动条上下半点击翻页、`menu scroll <offset> <max>` 日志。
- 第 7 行是动作：Enter → `menu: exit to terminal` → runner `exit to terminal (start menu)` → 回终端。
- 门禁 `scripts/gui_menu_scroll_acceptance.py` **13/13**（含滚动前第 7 行灰像素 0、滚动后青/蓝像素互换）。

### 3. 计算器：小数 / 百分比 / 内存 / 鼠标点按

- 定点 1/10000、4 位小数、饱和、无 f32；乘除 i128 中间值。
- `%`：`+`/`-` 且已键入操作数 → 累加器的 e%（`50+10%`=550000）；否则 `e/100`（`200*10%`=200000）。
- 内存 MC/MR/M+/M-（`C` 保留内存、`AC` 全清；有内存时画 accent `M`）；`±`/`DEL`/`CE`/`00`/`√`/`x²`/`1÷x`。
- 窗口 300×260 → **320×330**（5×6 键板），鼠标点按走 `calc_keypad_hit`+`calc_tap`，与键盘同一状态机。
- 门禁重写 `scripts/gui_calc_acceptance.py` **21/21**（7 组键盘算式 + 4 次鼠标点按 + 内存链 + 显示条/键板像素 + 无 PANIC）。

### 4. 垂直回扫探测 + 两套延迟直方图（并如实报告「QEMU 没有可用 vblank」）

- `kernel/gui/vsync.c/.h`：`vsync_init/available/present/hz100/stats/selftest` + 诊断接口
  （CRTC 寄存器、原始 1 秒采样、标称/实测速率、句柄化格式化）；`vsynctest` shell 命令。
- present 路径：每次上屏前一次有界等待（≤≈40 ms），`SwapState` 新增
  `vsync_edges/timeouts/unavail/vsync_hz100` + 等待直方图（<0.5/2/6/12/≥12 ms）+
  产出→上屏延迟直方图（<17/34/50/100/≥100 ms），30 帧一行 `[gui-shell] vsync: …`，`guishell swapstat` 同步带出。
- **实测结论**：`-display none` 与 `-display gtk` 下 `0x3DA` bit3 都是「每读一次翻转一次」
  （7 轮复现 `high` 恒等于 `samples/2`，实测 111259–280426 Hz，随轮询速度变化）。
  根因是 QEMU v11.1.0 `hw/display/vga.c:406-411` 的 `VGA_IS1_RC` 只做 toggle
  （`vga_dumb_retrace()`），精确回扫 `vga_precise_retrace()` 只在非默认 `vga_retrace_method==PRECISE` 下启用。
  → 驱动判 `source=none`、`vsync_hz100()==0`、`vsync_present()` 返回 -1；节拍继续用 HPET 16.67 ms 网格，
  每次上屏在直方图里记一次 `unavailable`。**真机 0x3DA bit3 是文档化的垂直回扫标志，代码按此实现，
  但本机无法验证真机行为，不外推。**
- 门禁 `scripts/gui_vsync_present_acceptance.py` **17/17**：探测证据（`[vsync] math:` PASS、
  `source=none`、实测速率 >1000 Hz、1 秒原始采样 `high>0`）+ present 侧守恒
  （`edges+timeouts+unavailable == presents`、等待直方图求和==edges、延迟直方图求和==presents-1、等待≤40 ms）。

---

## ■ 验证

**同一份 ISO、一次连续 `scripts/gui_sweep.py` 全量 31 门全绿（31/31 PASS）**，逐门：

| 门禁 | 结果 | 门禁 | 结果 |
|---|---|---|---|
| 静态渲染 static | 21/21 | 交互层 interactive | 13/13 |
| 桌面外壳 shell | 55/55 | 交换链 swap | 30/30 |
| 合成器 damage | 28/28 | C 桌面任务栏 apps_taskbar | 9/9 |
| 编辑器落盘 editorlive | 12/12 | 开始菜单 menulaunch | 10/10 |
| 图片浏览 imgbrowse | 9/9 | **计算器 calc** | **21/21** |
| **vsync 探测 vsync_present** | **17/17** | **键盘归属 keyowners** | **13/13** |
| **菜单滚动 menuscroll** | **13/13** | 桌面 desktop | 19/19 |
| 应用内容 apps_content | 23/23 | 外观（新桌面） | 0 像素（动态矩形外） |
| 外观（C 桌面） | 0 像素 | Logo 动画 logo | 14/14 |
| wl_server | 28/28 | wl_protocol | 29/29 |
| wl_multi | 40/40 | wl_semantics | 38/38 |
| wl_show | 9/9 | wl 桌面窗口 wl_window | 11/11 |
| wlprobe（ring-3 前置） | 14/14 | GUI 中文 gui_cjk | 4/4 |
| 图片查看器 imgview | PASS | C 编辑器 editor | 5/5 |
| AF_UNIX unix | 18/18 | shm 共享内存 | 18/18 |
| fd 传递 fdpass | 21/21 | | |

另外：开机自检 **16/16**；符号扫描 **1447 全局符号 / 0 冲突（145 对象）**；
`[vsync] math: … PASS`（速率策略/格式化/周期换算自检）与 `[vsync] init: source=none`
（诚实拒绝）出现在同一份日志里。

---

## ■ 产物

| 项 | 值 |
|---|---|
| 版本 | `v0.64.0-alpha` |
| tag | `v0.64.0-alpha`（附注标签，内容见 `build/tag-notes-v0.64.0-alpha.txt`） |
| ISO | `sutureos.iso` **23,824,384 B** |
| sha256 | `b89ede95c0443770db6b37de525efbf8d8277289ad9c2fe785e022edd7f4b7ab` |
| 内核 | `kernel.elf` **6,607,000 B** |
| ISO 内版本串 | `0.64.0-alpha` 命中 5 次、`0.63.0-alpha` / `0.59.0-alpha` 命中 0 次 |
| 下载直链 | https://github.com/Fanqi89/SutureOS/releases/download/v0.64.0-alpha/sutureos.iso |
| 发布页 | https://github.com/Fanqi89/SutureOS/releases/tag/v0.64.0-alpha |

---

## ■ 已知限制（照实写）

1. **没有真 vsync**：QEMU 上 `0x3DA` 是「每读翻转」的假信号（有源码行号证据），所以本版只是把
   **探测、有界等待、两套直方图**做实，节拍仍是 HPET 60 Hz 网格。真机未测、不外推。
   若将来某台机器报 `source=vretrace`，代码会用 `vsync_hz100()` 定网格（这一分支本机无法触发）。
2. 读 `0x3DA` 会复位 attribute controller flip-flop（QEMU 与真机同）：现在没有驱动碰 `0x3C0/0x3C1`，
   所以无副作用；将来若有驱动碰 AC 寄存器，不能与本采样交织。
3. `vsync_present()` 是 `inb` + `pause` 自旋，是寄存器轮询不是 scanline 中断，会占 CPU。
4. 计算器：entry 最长 12 字符；溢出用 i64 饱和（不报错）；`%` 与乘除都整数截断（不四舍五入）；
   无括号 / repeat-equals / 千分位 / 键盘自动重复；只处理鼠标左键。
5. 开始菜单：可视 6 行、按行滚动（不是像素级平滑滚动）；只有 1 个动作行；无滚轮支持
   （`mbos_mouse` 只做 3 字节包，滚轮未接线）。
6. 注册表仍受编译期数组限制（`WINS=6` / `KEY_OWNERS=4` / `MENU_ROWS=7`），加应用要改常量并重编。
7. 其它长期限制未变：ring-0 Rust 外壳（非 ring-3 合成器）、编辑器 10×38 仅 ASCII、图片只 BMP/QOI
   ≤256×256、Wayland 服务端只实现子集、桌面 Wayland pump 依赖单任务内核假设。
