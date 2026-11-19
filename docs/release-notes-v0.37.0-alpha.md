## 🚀 本次更新 (v0.36.0 → v0.37.0)

**GUI 重做的第一批：新风格的第一帧原型合入主仓**。`guislint` 命令用 **Rust 编写的软件光栅器**
把一张"圆角 + 轻亚克力 + 微透叠层 + Win11 Fluent 底 + 拟物化伪 3D 图标"的 1024×768 静态样板
真正画进 framebuffer，QEMU 截图逐像素验收 **13/13**。同时用 `--gc-sections` 把 Slint 静态库带来的
+2.24 MB 压到 **+0.65 MB**。

> ⚠️ 如实说明：渲染器是 **手写 Rust 软光栅器**，不是 Slint。Slint 1.18 在内核里能链接、能建平台/组件、
> timer 能跑（最小场景"底色+圆角矩形"能画完），但 `SoftwareRenderer::render()` 一遇到"多图元+渐变/阴影"
> 就不返回（死循环，非崩溃），带 `Text` 元素更早 panic（`item_tree.rs:1414` 越界）。按时间盒降级，
> Slint 后端完整保留在 `--features slint` 下可复现。

---

### 📦 新增：`guislint` 与 Rust GUI 栈

| 组件 | 内容 |
|---|---|
| `rust/suture-gui/` | `no_std` staticlib（`x86_64-unknown-none`）：`src/swrast.rs`（软光栅器 354 行：圆角矩形/渐变/alpha 合成/4 层假阴影/伪 3D 高光带）+ `src/lib.rs`（平台适配 360 行）+ 两个 `.slint` |
| `kernel/gui/slint_bridge.c/.h` | C↔Rust 边界：取 framebuffer 指针/pitch、导出 `slint_gui_render_frame()` / `slint_gui_linked()` |
| `kernel/shell/shell.c` | 新命令 `guislint`：画一帧并保持（ESC 或 10 s 返回终端；画帧期间挂起终端避免日志盖画面） |
| `scripts/build.py` | 构建 Rust GUI 静态库 → 从 ar 归档提取**全部 417 个对象**（含 compiler_builtins，软浮点 intrinsic 必需）→ 命令行走 `@link.rsp` 响应文件（Windows 32767 字符上限）→ 加 `--gc-sections` |
| `scripts/slint_gui_acceptance.py` | 真 QEMU + screendump 像素断言（272 行） |

### ✅ 验收（真 QEMU 帧缓冲 + 逐像素断言）

| 项目 | 结果 |
|---|---|
| 帧真的画出来 | 串口 `[gui-slint] frame rendered 1024x768 checksum=917e8f72 frames=1 OK`，无 PANIC，命令正常回终端 |
| 圆角 | 四角像素 vs 面板填充 \|d\| = 69 / 67 / 73 / 107 |
| 半透明叠层 | 同一面板压两种背景 \|d\| = 196（(91,91,166) vs (45,49,58)） |
| 假阴影梯度 | 亮度 68.3 → 89.3 → 102.0（越远越接近背景） |
| 伪 3D 图标 | 最亮行在第 5/72 行（顶部 45%），高光 223.4 vs 面板 113.8 |
| 脚本 | `scripts/slint_gui_acceptance.py` → **13/13 PASS** |
| 体积 | kernel.elf 4,956,200 → **3,364,744 B**（`--gc-sections` 省 1.59 MB） |
| 回归 | C 桌面 `apps_taskbar` **9/9**、GUI 外观门禁 **0 像素**、内核自检 13/14、`[rust] core ... PASS`、FAT32 宿主 21/21 |

截图：`docs/screenshots/slint-prototype-v0.37.0.png`（1024×768）。

### ⚠️ 已知限制（如实记录）

1. **没有文字**：Slint `Text` 会 panic，光栅器本轮也没做字形 → 下一批直接复用 `kernel/term/cjk16.c` 的 16×16 GB2312 点阵。
2. 单帧静态：没有输入、没有动效、没有窗口管理；`guislint` 只是"画一帧按住"的演示命令。
3. **没有真模糊**：只有半透明 + 微透叠层 + 高光，"轻亚克力"的磨砂模糊要离屏 box-blur（下一批）。
4. 两个彩色色块是**探针**不是最终美术：只为让"半透明合成"可被像素断言。
5. 只支持 32bpp 帧缓冲（24bpp 会如实报 `need a 32bpp framebuffer`）；fps 未测。
6. Rust GUI 栈仍编译进内核（ring 0），不是 ring-3 进程；Wayland/AF_UNIX/poll/memfd 前置能力未做（见路线图 B24）。
7. 启动呈现与 C 桌面未变（外观门禁 0 像素）。

### 📜 出处与许可（本版）

- 渲染器与平台适配为 SutureOS 自有实现（GPL-3.0）；依赖为 Rust 生态 crate（MIT/Apache-2.0 双授权，经 cargo 下载），
  Slint 本体为 GPL-3.0/商业双授权（本项目 GPL-3.0 兼容）；未拷贝任何第三方 UI 资源。

### 📦 下载

- 附件 `sutureos.iso`（17336320 字节）
- sha256：`fab69958a3b40cde83b2c34e9b09d24ca808635c240648232d45e0d459c29fc9`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.37.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.37.0-alpha
