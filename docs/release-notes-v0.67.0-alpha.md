# SutureOS v0.67.0-alpha

**输入层改成批量投递：一次快速打字不再丢键，而且这一改把一个潜伏的 Rust 越界 panic 当场打出来了。**

上一版（v0.66）的编辑器门禁被迫把按键间隔放到 **0.45 秒/键**才稳定——那不是键盘的问题，是 ABI 的问题：桥每帧只往外壳递 **一个** 按键事件。这一版把它改成「内核按事件推、外壳每帧排空」。

---

## ■ 关键修复（现象 → 根因 → 怎么修）

| 现象 | 根因 | 怎么修 |
|---|---|---|
| 打字稍快就丢键：v0.66 门禁必须 0.45 s/键 才稳定，更快的 burst 会静默丢字符 | 桥的帧循环每帧从 32 项事件队列里只取 **一个** 事件交给 `suture_gui_shell_step(..., key, ch)`，其余留在队列里等下一帧；TCG 下每帧 100–300 ms，30 ms/键的输入量远大于消费速度 → 队列满后丢最旧事件 | 新增 **push ABI**：`suture_gui_shell_push_key(key, ch, mods)` 把事件追加到外壳里 64 项队列，`shell_step` 在渲染前 **排空整条队列**（每个事件都走原来的 per-key 逻辑，逻辑被抽成 `handle_key()`）；桥把这帧读到的所有事件全部推入，一帧只渲染一次 |
| 快速投递一上来就 `[gui-slint] PANIC in Rust GUI`（撤销/回车连发后立刻死） | `ed_maybe_snapshot()` 在 4 个撤销槽满后把索引 `min` 到 `ED_UNDO_DEPTH`（=4），下一次编辑却写 `ED_UNDO[4]`——4 元素数组越界 → Rust panic。v0.66 每次只处理一个键、快照很少连续填满，所以从没触发 | 满时先把最老的快照整体前移一格（i+1 → i）再写最后一个槽，变成真正的 4 级环形语义；慢速输入下行为不变 |
| `Ctrl+Shift+Z` 没有语义 | 解码器只折叠 Ctrl+字母（0x1A 等），Shift 被忽略 | `Ctrl+Shift+<字母>` 返回 `0x80 \| 控制码`（`Ctrl+Shift+Z` = 0x9A），编辑器新增该绑定为「重做」，与 Ctrl+Y 并存 |
| 队列一旦溢出只能靠肉眼发现 | 没有计数器/日志 | 溢出时丢最旧并 **打一次** `[gui-shell] key queue overflow (N dropped)`；门禁把「日志里出现这行」判为失败 |

---

## ■ 新增

### 1. 批量按键投递（push ABI）

- `kernel/gui/slint_bridge.c`：每帧把 `kq[]` 里读到的**全部**事件 `suture_gui_shell_push_key()` 进外壳，再调一次 `suture_gui_shell_step()`；`shell_step` 的参数从 16 个减回 14 个（`key/ch/mods` 移到 push 接口）。
- `rust/suture-gui/src/shell.rs`：新增 64 项按键队列 `KEYQ`（key, ch, mods）+ `push_key()`；把原来内联在 `shell_step` 里的大段按键处理抽成 `handle_key(sh, key, ch, mods)`，帧内按顺序排空。
- 效果：**30 ms/键（约 33 键/秒）连续输入零丢失**，且一帧只渲染一次（不会因为每个键都重绘而卡顿）。

### 2. 撤销栈越界 panic（真 bug）

- `apps.rs` 的 `ed_maybe_snapshot()`：4 级满后改为「丢最老、其余前移一格再写」，不再越界；`ED_UNDO_N` 始终 ≤ 4。

### 3. `Ctrl+Shift+Z` 重做

- `mb_kbd.c`：`Ctrl+Shift+<字母>` → `0x80 | (base & 0x1f)`；`shell.rs` 的 `editor_keys` 把 0x9A 接到 `editor_redo()`（日志仍是 `editor ctrl <sc> <ch>`，门禁能看到 `ch=154`）。

### 4. 门禁强化：`gui_editor_live_acceptance.py` **19/19**

- 按键间隔从 0.45 s 降到 **0.03 s**；文档里新增 11 个字符的快速 burst（`quick brown`）。
- 新增两项：**「shell 从未丢过队列按键」**（断言无 `key queue overflow` 行）与 **「Ctrl+Shift+Z 到达 editor_redo」**（`editor ctrl 44 154`）。
- 仍然逐字节比对宿主解析的 FAT32 镜像（这次 19 行 / **71 字节**）、检查视口滚动与滚轮回滚、PageUp 落点。

---

## ■ 验证

**同一份 ISO、一次连续 `scripts/gui_sweep.py` 全量 32 门全绿（32/32 PASS）**：

| 门禁 | 结果 | 门禁 | 结果 |
|---|---|---|---|
| 静态渲染 static | 21/21 | 交互层 interactive | 13/13 |
| 桌面外壳 shell | 55/55 | 交换链 swap | 30/30 |
| 合成器 damage | 28/28 | C 桌面任务栏 apps_taskbar | 9/9 |
| **编辑器落盘 editorlive（0.03 s/键 + Ctrl+Shift+Z）** | **19/19** | 开始菜单 menulaunch | 10/10 |
| 图片浏览 imgbrowse | 9/9 | 计算器 calc | 28/28 |
| 滚轮 wheel | 9/9 | vsync 探测 vsync_present | 17/17 |
| 键盘归属 keyowners | 13/13 | 菜单滚动 menuscroll | 13/13 |
| 桌面 desktop | 19/19 | 应用内容 apps_content | 23/23 |
| 外观（新桌面） | 0 像素 | 外观（C 桌面） | 0 像素 |
| Logo logo | 14/14 | wl_server | 28/28 |
| wl_protocol | 29/29 | wl_multi | 40/40 |
| wl_semantics | 38/38 | wl_show | 9/9 |
| wl 桌面窗口 wl_window | 11/11 | wlprobe | 14/14 |
| GUI 中文 gui_cjk | 4/4 | 图片查看器 imgview | PASS |
| C 编辑器 editor | 5/5 | AF_UNIX unix | 18/18 |
| shm 共享内存 | 18/18 | fd 传递 fdpass | 21/21 |

另外：开机自检 **16/16**；符号扫描 **1453 全局符号 / 0 冲突（145 对象）**；
编辑器门禁本身证明了三件事：**0.03 s/键的连续输入零丢失**（日志里没有 `key queue overflow`）、
**Ctrl+Shift+Z 到达 `editor_redo`**（`editor ctrl 44 154`）、**19 行 / 71 字节文档在宿主解析的镜像里逐字节正确**。

---

## ■ 产物

| 项 | 值 |
|---|---|
| 版本 | `v0.67.0-alpha` |
| tag | `v0.67.0-alpha`（附注标签，内容见 `build/tag-notes-v0.67.0-alpha.txt`） |
| ISO | `sutureos.iso` **23,853,056 B** |
| sha256 | `49a56f323a209da2dc9a176bc9b8ab9e0ac8e1d0b04afb7a1692e2684b069d51` |
| 内核 | `kernel.elf` **6,622,944 B** |
| ISO 内版本串 | `0.67.0-alpha` 命中 5 次、`0.66.0-alpha` 命中 0 次 |
| 下载直链 | https://github.com/Fanqi89/SutureOS/releases/download/v0.67.0-alpha/sutureos.iso |
| 发布页 | https://github.com/Fanqi89/SutureOS/releases/tag/v0.67.0-alpha |

---

## ■ 已知限制（照实写）

1. 队列上限 **64 个事件/帧**；真的超了会丢最旧的并打一次 overflow 日志（门禁断言零溢出，但极端情形不假装无损）。
2. 一帧内多个按键只渲染**一次**：状态全部推进，但需要「逐键视觉反馈」的场景（例如逐键动画）看不到中间帧。
3. 修饰键覆盖仍然有限：Ctrl+字母（含 Ctrl+Shift+字母）有控制码；Alt 组合、非字母键的 Ctrl 组合仍按普通键处理；键盘依旧是轮询为主 + IRQ1 增强的混合路径（IRQ1 在本机 QEMU 上只送约 2 个中断）。
4. 撤销仍是 **4 级**，满时丢最老的一步；合并粒度是「连续同类操作」。
5. 编辑器仍只接受 ASCII（0x20–0x7E），无输入法、无 CJK 输入路径。
6. QMP 侧只能注入能映射成 qcode 的字符（大写字母要用 `shift-<字母>`），所以门禁文案用小写。
