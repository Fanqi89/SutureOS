## 🚀 本次更新 (v0.48.0 → v0.49.0)

**关闭/最小化也有过渡了，并且给 "Slint 还是自写渲染器" 这条路线正式定了稿。**
关闭与最小化现在是**先 2 帧淡出、再真正执行动作**（延迟执行），外壳验收扩到 **38/38**；
同时新增 `docs/slint-route-decision.md`，把 13 个批次里攒下的证据写成结论文件。

---

### ✨ 关闭/最小化淡出（v0.49.0）

| 项 | 做法 |
|---|---|
| 状态 | `Shell::{act_frames, act_kind, act_idx}`，`begin_act(kind, idx)` 置 2 帧（kind 1=最小化，2=关闭） |
| 绘制 | 目标窗口叠黑色 `rect_fill`，alpha `0x2c → 0x64`，两帧后执行真正的 `minimized = true` / `open = false` |
| 为什么延迟 | 立即隐藏就没有动画可画；延迟两帧让"淡出→消失"成立，动作完成后再重排焦点 |
| 日志 | `[gui-shell] anim minimize N` / `anim close N` → `[gui-shell] anim done K N` → 既有 `minimize`/`close` |
| 空闲跳过 | 动画期间**不跳过**空闲帧（与 v0.48 打开淡入同一规则） |

### 📄 Slint 路线结论（对应总目标 ⑥）

新增 `docs/slint-route-decision.md`，结论：**长期走自写渲染器**，Slint 只保留为可复现参考后端
（`cargo build --features slint`）。三条硬证据：① `SoftwareRenderer::render()` 在多图元+渐变/阴影场景
**死循环不返回**；② `Text` 在 `i-slint-core-1.18.1/item_tree.rs:1414` **panic**；③ 无 SSE 目标上
`f32` 全是软浮点，逐像素 `fsqrt` 就值 ~420 ms/窗口——**查表 + 整数**才是唯一可行路线（v0.47 已证明：
窗口阶段 −80%、整屏重绘 ≈117 ms）。重新评估的条件也写进了文档（等 SutureOS 具备 std/线程/epoll 之后）。

### ✅ 验收

| 项目 | 结果 |
|---|---|
| 外壳 `slint_gui_shell_acceptance.py` | **38/38 PASS**（原 37 项 + 关闭淡出） |
| 淡出证据 | `anim close 2` + `anim done 2 2`；随后 `close 2` 正常触发，窗口区域回落到基线 |
| 回归 | 静态像素 **21/21**、交互 **13/13**、C 桌面 `apps_taskbar` **9/9**、GUI 外观门禁 **0 像素**、内核 13/14 |
| 性能 | `profile/30 ... win=282 bar=28 ticks`（v0.47 280，未回退） |

截图：`docs/screenshots/shell-prototype-v0.49.0.png`。

### ⚠️ 已知限制（如实记录）

1. 淡出是**整块矩形压暗**（不是真透明度合成），2 帧在 ~13 fps 下约 20–30 ms 墙钟（QEMU 里偏短）。
2. 最大化/还原、开始菜单展开仍无动画。
3. 拖动窗口仍会重建窗口缓存（几何每帧变）——**图层合成器（B36）尚未开始**，这是下一个大件。
4. 时区写死 +08:00，无 TZ 数据库/夏令时；圆角覆盖率为整数像素查表（1 px 级 AA）。
5. 键盘导航只在开始菜单内；窗口内容无键盘焦点。

### 📜 出处与许可（本版）

- 全部为 SutureOS 自有实现（GPL-3.0），未拷贝第三方代码。

### 📦 下载

- 附件 `sutureos.iso`（17496064 字节）
- sha256：`be94c9c8214f4b0ec4f074a26f9b5214c4fb617eff9a98b4e9e1850d80654431`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.49.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.49.0-alpha
