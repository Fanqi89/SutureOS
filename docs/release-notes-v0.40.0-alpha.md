## 🚀 本次更新 (v0.39.0 → v0.40.0)

**新渲染器活了**：`guislint` 从"画一帧按住 10 秒"变成**真交互**——真实 PS/2 鼠标移动会让列表行高亮
（含 Fluent 蓝色指示条）、真实点击会画扩散涟漪、指针自带箭头光标、面板淡入；ESC 干净退出。
交互验收 **13/13**（全新脚本，QMP 注入真鼠标/键盘），静态像素验收 **21/21** 保持全绿。

---

### 📦 交互层（v0.40.0）

| 组件 | 内容 |
|---|---|
| `kernel/gui/slint_bridge.c` | `slint_gui_run_interactive()`：内核轮询 PS/2 鼠标/键盘 → 每帧把指针位置、按键、点击边沿、毫秒时钟交给 Rust；30 fps 有界自旋节流；ESC 退出；帧数安全上限 |
| `rust/suture-gui/src/lib.rs` | `suture_gui_interactive_step()`：静态场景**缓存**（一次渲染，见下）+ 每帧只合成动态层（悬停高亮/指示条、点击涟漪、光标）；`suture_gui_cache_seed()` 把静态首帧直接灌进缓存 |
| `rust/suture-gui/src/swrast.rs` | `cursor()`：白底深描边箭头光标 |
| 动效 | 450 ms 面板淡入（按毫秒时钟插值）；450 ms 点击涟漪（半径 6→52 px，透明度衰减）；悬停行亮度提升 + 左侧 3 px 蓝色指示条 |
| 性能 | 一次全场景渲染在 TCG 下约 1.5 s（解析 AA 圆角 + 4 层阴影每像素一次 sqrt）→ 静态场景缓存成 1024×768 u32（3 MiB `.bss`），每帧 memcpy + 小面积动态层，帧率从"每 1.5 s 一帧"到可交互 |
| `scripts/slint_gui_interactive_acceptance.py` | 新验收：真 QMP 鼠标移动/点击/ESC，日志 + 像素双重断言（13 项） |

### ✅ 验收

| 项目 | 结果 |
|---|---|
| 交互脚本 `slint_gui_interactive_acceptance.py` | **13/13 PASS** |
| 交互循环启动 | `[gui-slint] interactive: loop (mouse=1), ESC exits` |
| 悬停路由 | `[gui-slint] hover row 0` / `hover row 1`（真实 PS/2 移动） |
| 悬停像素 | 悬停行亮度 144.8 vs 空闲行 119.5；换行后反过来（147.6 vs 120.7） |
| 点击路由 + 涟漪 | `[gui-slint] click at`；点击点 40 px 内 6400 px 变化 |
| 光标 | 新旧指针位置各 528 px 变化 |
| 退出 | `interactive: exit after N frames` + 终端恢复 |
| 静态像素回归 | `slint_gui_acceptance.py` **21/21**（圆角/亚克力/文字/伪 3D 全保持） |
| 其余回归 | C 桌面 `apps_taskbar` **9/9**、GUI 外观门禁 **0 像素**、内核 13/14 |

截图：`docs/screenshots/slint-prototype-v0.40.0.png`（悬停"显示与外观"行 + 光标）。

### ⚠️ 已知限制（如实记录）

1. 交互范围只有**两行列表的悬停、点击涟漪和光标**；图标/卡片/文字还不可点，没有页面切换。
2. 30 fps 是"有界自旋"实现的，没有 vsync/双缓冲；重绘时可能看到撕裂（缓存 memcpy 3 MiB 在 TCG 下不便宜）。
3. 场景缓存假设 1024×768；帧缓冲更大时回退到"每帧整场景渲染"的慢路径（会明显掉帧）。
4. 没有键盘焦点/输入框；只有 ESC 被消费。
5. 点击态、按下态、窗口过渡等更细的动效还没做；没有亚克力噪点。
6. 仍是 prototype：没有窗口管理，不与 C 桌面共享状态。

### 📜 出处与许可（本版）

- 交互/动效/光标为 SutureOS 自有实现（GPL-3.0），未拷贝第三方代码。

### 📦 下载

- 附件 `sutureos.iso`（17438720 字节）
- sha256：`223b9f81dd8b08962e10bf457bfb1ada730dad3896354c10037d089302122894`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.40.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.40.0-alpha
