## 🚀 本次更新 (v0.42.0 → v0.43.0)

**开始菜单可以用键盘了**：菜单打开后 **↑/↓ 移动选择**（选中行有独立底色）、**Enter 启动所选应用**
（和鼠标点击走同一条 `launch_window()` 路径），鼠标悬停高亮与键盘选择互不干扰。外壳验收扩到 **32/32**
（真 QEMU 键盘 `sendkey down/down/ret`），静态 21/21、交互 13/13、C 桌面 9/9 全保持绿。

---

### 📦 键盘菜单（v0.43.0）

| 能力 | 行为 |
|---|---|
| ↑ / ↓ | 在菜单项间循环移动选择（0→1→…→0），日志 `[gui-shell] menu sel N` |
| Enter | 启动/聚焦选中的窗口，日志 `[gui-shell] menu enter N` + `menu launch N`，菜单关闭 |
| 鼠标 | 悬停照旧高亮；选中项用较暗底色，避免和悬停混淆 |
| 键盘事件来源 | C 侧 `slint_gui_run_shell()` 每帧把最后一个 make-code 按键传入 Rust（ESC 仍由 C 侧处理并退出），参数为 `suture_gui_shell_step(..., uint32 now_ms, int32 key)` |

### ✅ 验收

| 项目 | 结果 |
|---|---|
| 外壳 `slint_gui_shell_acceptance.py` | **32/32 PASS**（原 30 项 + 键盘选择 + Enter 启动） |
| 键盘选择 | `menu sel 1` → `menu sel 2`（两次 Down） |
| Enter 启动 | `menu enter 2` + `menu launch 2`；图片查看器窗口出现（该区域亮度 124.6） |
| 原有交互 | 鼠标拖动/最小化/恢复/双击最大化/关闭/空闲跳帧/ESC 全部保持通过 |
| 回归 | 静态像素 **21/21**、交互 **13/13**、C 桌面 `apps_taskbar` **9/9**、外观门禁 **0 像素** |

截图：`docs/screenshots/shell-prototype-v0.43.0.png`（开始菜单打开、键盘选中项高亮）。

### ⚠️ 已知限制（如实记录）

1. 键盘导航只在开始菜单内生效；窗口内容仍不可用键盘操作（没有文本框/焦点链）。
2. 菜单只有 ↑/↓/Enter，没有 Home/End/Tab/Esc 语义细分（Esc 仍是"退出外壳"）。
3. 一次只传"本帧最后一个按键"——极快连按可能丢中间键。
4. **脏矩形的尝试本轮失败并回退**：按脏矩形局部重绘后开始菜单/新窗口区域会变黑（调查中：`Surface::clip` 已生效，但窗口/磨砂的多段绘制路径对局部裁剪的假设没对齐），为避免交付退化，本轮整体回退到 v0.42 的整屏重绘，交互帧率仍是约 2 fps。
5. 最大/最小化、菜单启动都还没有过渡动画；窗口页脚版本串已更新到 v0.43。

### 📜 出处与许可（本版）

- 全部为 SutureOS 自有实现（GPL-3.0），未拷贝第三方代码。

### 📦 下载

- 附件 `sutureos.iso`（17483776 字节）
- sha256：`2bb3348b78cbecf5d8474860ef2067875ceb2691509316080747d2b8967b07e4`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.43.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.43.0-alpha
