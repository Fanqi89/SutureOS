# SutureOS v0.65.0-alpha

**输入补齐：PS/2 滚轮真的接进桌面（菜单一格一行、图片查看器翻页），计算器补上括号、连按等号与千分位。**

这一版没有碰启动呈现，也没有改默认桌面的像素基线（只有「关于本机」的版本串变了，基线已按规矩重生）。改动集中在两处：鼠标驱动的输入层，和计算器的表达式能力。

---

## ■ 关键修复（现象 → 根因 → 怎么修）

| 现象 | 根因 | 怎么修 |
|---|---|---|
| 滚轮完全没反应，`mouseinfo` 只显示 "wheel mouse" 却从不产生事件 | 驱动把包长度写死成 3 字节：设备即使在 IntelliMouse 模式也按 4 字节发，第 4 字节（Z 半字节 + 额外键）被当成下一包的首字节，整条流失去同步 | `mb_mouse.c` 加标准 IntelliMouse 握手（0xF5 停止上报 → 魔术采样率 200/100/80 → 0xF2 重读 id），id=3/4 才切 4 字节包，否则保持 3 字节；第 4 字节按 **4 位补码**取 Z（`(int8_t)b >> 4`）累加，`mbos_mouse_take_wheel()` 每帧取一次、自检可注入 |
| 菜单只能键盘/滚动条翻，滚轮没反应 | 外壳的帧步进 ABI 里根本没有滚轮参数（`suture_gui_shell_step` 只有指针/点击/键盘） | ABI 加 `wheel`（C 侧每帧 `mbos_mouse_take_wheel()`），Rust 外壳：菜单打开时一格一行（单帧最多 3 格）、图片查看器聚焦时上滚=上一张/下滚=下一张；再补 PageUp/PageDown 翻整页 |
| 计算器算不了 `(2+3)*4` | v0.64 的模型是「一个累加器 + 一个待定运算符」的平面结构，没有括号位置 | 加 8 层括号栈（每层存一份 累加器+运算符）：`(` 把当前状态压栈并开新子式、`)` 结算子式并把结果当外层操作数；`=` 自动闭合所有未闭合层。模型仍是严格左到右（与平面模型一致，不假装有优先级） |
| 连按 `=` 什么也不发生（`5+3=`、`=`、`=` 停在 8） | `=` 结算后把待定运算符清零，没有记住「上次运算」 | 记住 `last_op/last_rhs`，裸 `=` 时重复应用；首次 `=` 与 `42=`（只采纳键入值）不设置重复态；清屏/`AC`/错误都清掉重复态 |
| 大数难读（`1000000`） | 显示只做定点格式化，没有分组 | 结算值按三位加 `,`（示例 `1,000,000`），超过显示宽度（13 列）自动退回纯数字；键入过程不加分隔符（保持输入原样）。同时打 `calc show <text>` 让门禁逐字核对而不是 OCR |
| `wl_multi` 门禁偶发失败：`wl_pointer.button` press/release 一条都没有（33/40） | Wayland 泵每轮**采样一次**按键掩码，两轮之间发生的 press+release 就永远看不到；而 QMP monitor 的 `mouse_button 1;mouse_button 0` 是紧贴着发的 | 把按键变化做成**边沿队列**：`mb_mouse` 每次按键变化入队（16 项），`mbos_mouse_take_button()` 逐条取出；桌面泵与终端泵都按队列回放，一次 press+release 不会再被采样掉。修复后同一门禁 **40/40** 稳定通过 |

附带的两个实测结论（写在这里因为都影响实现）：

- **QEMU 的 PS/2 模型只投递 wheel-up（z=−1），wheel-down 完全不产生包**：三次独立探针（只发 down / 只发 up / 交替发）里 down 一次都没到 guest。于是把「正方向」的验证放进内核：`mouseinfo wheel` 用合成 4 字节包走**真实解码器**，覆盖 z=+1、z=−1 与跨包累加。
- **PS/2 的 Z 约定是「负 = 远离用户（上滚）」**：我第一版按 QMP 的直觉写成了反的，门禁（滚轮上滚应该让菜单回到第 0 行）当场抓到并修正。

---

## ■ 新增

### 1. PS/2 滚轮全链路

- `kernel/drivers/mb_mouse.c/.h`：IntelliMouse 握手、4 字节包解码、Z 累加（±64 夹紧）、`mbos_mouse_take_wheel/device_id/packet_size/wheel_events`、`mouseinfo wheel` 合成包自检（z=+1、z=−1、累加）；启动日志 `[mouse] device id=0x3 (wheel) packet=4 bytes`，每个真实滚轮事件打 `[mouse] wheel z=<n>`。
- `kernel/gui/slint_bridge.c` + `rust/suture-gui/src/shell.rs`：帧步进 ABI 加 `wheel`；菜单一格一行（`menu scroll`）、图片查看器上滚/下滚切换文件（`image shift 2/1` + `image: loaded …`）；日志 `[gui-shell] wheel <delta> menu=<0/1>`。
- 门禁 `scripts/gui_wheel_acceptance.py` **9/9**：id=0x3/4 字节包、合成包双向自检 PASS、菜单滚轮回到第 0 行、图片查看器滚轮切到 `IMG.QOI`、滚动后第 7 行真的画在最后一格（灰 accent 像素 352）。

### 2. 菜单分页键

- PageUp/PageDown 一次翻「可视行数」（6 行），不移动选择；与 v0.64 的 ↑/↓/Home/End/滚动条并存。

### 3. 计算器：括号 / 重复等号 / 千分位

- 8 层括号栈，`=` 自动闭合；日志 `calc paren <深度>`。
- 重复等号：`5+3=` → 8，`=` → 11，`=` → 14（`calc repeat 43`）。
- 千分位显示 + `calc show <text>` 日志（`calc show 1,000,000`）。
- 键盘板 6 行 → **7 行 / 35 槽**（新行放 `(` `)`，其余空槽不绘制不响应），窗口 330 → **360 px**，格子仍 36 px 高；`calc_layout()` = `(7, 5, 16, 92, 57, 36)`。
- 门禁 `scripts/gui_calc_acceptance.py` 扩到 **28/28**：v0.64 的 21 项全保留，新增括号（键盘 `(2+3)*4=`→200000、`50/(2+3)=`→100000、键板点 `(` `)` → 30000）、重复等号三连（80000/110000/140000 + `calc repeat 43` ×2）、千分位（`calc show 1,000,000`）、键板 35 槽与 36 px 格高。

---

## ■ 验证

**同一份 ISO、一次连续 `scripts/gui_sweep.py` 全量 32 门全绿（32/32 PASS）**：

| 门禁 | 结果 | 门禁 | 结果 |
|---|---|---|---|
| 静态渲染 static | 21/21 | 交互层 interactive | 13/13 |
| 桌面外壳 shell | 55/55 | 交换链 swap | 30/30 |
| 合成器 damage | 28/28 | C 桌面任务栏 apps_taskbar | 9/9 |
| 编辑器落盘 editorlive | 12/12 | 开始菜单 menulaunch | 10/10 |
| 图片浏览 imgbrowse | 9/9 | **计算器 calc** | **28/28** |
| **滚轮 wheel** | **9/9** | vsync 探测 vsync_present | 17/17 |
| 键盘归属 keyowners | 13/13 | 菜单滚动 menuscroll | 13/13 |
| 桌面 desktop | 19/19 | 应用内容 apps_content | 23/23 |
| 外观（新桌面） | 0 像素 | 外观（C 桌面） | 0 像素 |
| Logo logo | 14/14 | wl_server | 28/28 |
| wl_protocol | 29/29 | **wl_multi（按键边沿队列修复后）** | **40/40** |
| wl_semantics | 38/38 | wl_show | 9/9 |
| wl 桌面窗口 wl_window | 11/11 | wlprobe | 14/14 |
| GUI 中文 gui_cjk | 4/4 | 图片查看器 imgview | PASS |
| C 编辑器 editor | 5/5 | AF_UNIX unix | 18/18 |
| shm 共享内存 | 18/18 | fd 传递 fdpass | 21/21 |

另外：开机自检 **16/16**；符号扫描 **1453 全局符号 / 0 冲突（145 对象）**；
`mouseinfo wheel` 的合成包自检 `z=+1, z=-1, accumulate -> PASS` 与
`[mouse] device id=0x3 (wheel) packet=4 bytes` 出现在同一份日志里。

---

## ■ 产物

| 项 | 值 |
|---|---|
| 版本 | `v0.65.0-alpha` |
| tag | `v0.65.0-alpha`（附注标签，内容见 `build/tag-notes-v0.65.0-alpha.txt`） |
| ISO | `sutureos.iso` **23,848,960 B** |
| sha256 | `3317a38ae2ac4c3b88635d31de0d4b9e6b8a41b7077d546b46026de3155684fb` |
| 内核 | `kernel.elf` **6,620,552 B** |
| ISO 内版本串 | `0.65.0-alpha` 命中 5 次、`0.64.0-alpha` / `0.63.0-alpha` 命中 0 次 |
| 下载直链 | https://github.com/Fanqi89/SutureOS/releases/download/v0.65.0-alpha/sutureos.iso |
| 发布页 | https://github.com/Fanqi89/SutureOS/releases/tag/v0.65.0-alpha |

---

## ■ 已知限制（照实写）

1. **滚轮的下滚方向没有端到端证据**：QEMU 的 PS/2 模型只投递 wheel-up（三次探针记录在 `docs/wheel.md`），所以真机/模拟器上的「下滚」只由合成包自检（同一解码器）覆盖，没有真实设备注入。真机未测、不外推。
2. 滚轮是**离散一格一行**，没有平滑滚动、惯性或加速；单帧最多消费 3 格（防止重绘抖动时一次跳太远）。
3. 菜单仍是按行滚动（不是像素级平滑），滚轮只在菜单打开时生效；图片查看器只在它拥有焦点时吃滚轮；编辑器仍无滚动。
4. 计算器括号最多 8 层、严格左到右（不实现运算优先级）；`%` 语义与 v0.64 相同（+/- 取累加器的百分比，否则 e/100）；千分位只在结果能放进 13 列时显示；`calc show` 只是日志，屏幕上仍是同一份文本。
5. `mouseinfo wheel` 的自检是**报文层**的（合成包进真实解码器），不等于真机滚轮已被验证。
