# SutureOS v0.66.0-alpha

**桌面编辑器做实：48×64 缓冲、滚动视口、撤销/重做，以及「Ctrl 键终于可用」——两条都是门禁逼出来的真 bug。**

这一版没有动桌面视觉（外观基线按发布纪律重生，动态矩形外 0 像素）。改动集中在两处：编辑器窗口的缓冲区/视口，和 PS/2 键盘解码器的修饰键语义。

---

## ■ 关键修复（现象 → 根因 → 怎么修）

| 现象 | 根因 | 怎么修 |
|---|---|---|
| 编辑器只能装 10 行 × 38 列，行一多就没法看，也没有任何滚动 | 缓冲区是 `[[u8;38];10]` 的固定小数组，绘制永远从第 0 行开始，没有「视口起点」概念 | 缓冲区 48×64（3 KiB），新增 `ED_TOP` 视口起点：窗口只画 8 行（330×230 扣标题栏/状态栏），右侧 3 px 滚动条，状态栏 `Ln x/y Col c`；**光标移动/插入会自动把视口拉回光标**，滚轮与 PageUp/PageDown 只移动视口不移动光标 |
| 按 Ctrl+Z 没撤销，反而在正文里插了一个 `z`（实测保存出来的文件是 `abczzyzz`） | 外壳每帧只取一次 `mbos_kbd_mod_state()` 快照，而 QMP 把 `ctrl down / z down / z up / ctrl up` **一次性发完**；等外壳从按键队列里取出这封信，Ctrl 早已释放 → mods=0，字母照常插入 | 在 **PS/2 解码器**里把 Ctrl+字母折成经典 ASCII 控制码（Ctrl+Z=0x1A、Ctrl+Y=0x19、Ctrl+S=0x13）：修饰键状态由**同一个字节**生效，不存在跨帧竞态；外壳按控制码路由 `editor_undo / editor_redo / editor_save` |
| 撤销/重做完全没有 | 缓冲区没有历史 | 4 级全快照（整缓冲 + 每行长度 + 光标 = 3 KiB/级；undo/redo 各 4 级在 .bss）；**连续同类操作合并成一步**（连打一段字算一次撤销、连按退格算一次），移动光标/换行则切分历史 |
| 视口滚动在日志里看不见（门禁无法断言） | `ed_ensure_visible()` 改 `ED_TOP` 时**不打日志**，只有滚轮/翻页打 | 视口每次变化都打 `[gui-shell] editor scroll <top>`，包含「跟随光标」的自动滚动 |
| 门禁按 10 行可见写预期，实际只显示 8 行 | 330×230 窗口扣掉标题栏（30）与状态栏/边距：`(230-34-40)/18 = 8` | 按真实几何修正（可视行数是 8，PageUp 一页 = 8 行） |

---

## ■ 新增

### 1. 编辑器缓冲区与视口

- `apps.rs`：`ED_LINES 10→48`、`ED_COLS 38→64`；新增 `ED_TOP`（视口起点）、`ED_VIEW`（当前帧实际画了几行，由窗口体回填）、`editor_scroll(delta)`、`editor_top()`。
- `editor_move` 增加 7=PageUp / 8=PageDown（一页 = 可视行数），所有移动后调 `ed_ensure_visible()`。
- `editor_body`：行号槽位加宽到 28 px，当前行高亮，光标块 2 px，右下角滚动条（滑块高度按行数比例），状态栏 `Ln 12/16 Col 3 ・ F5 ・ ^Z/^Y`。
- 启动日志 `[gui-shell] editor init lines=48 cols=64`。

### 2. 撤销 / 重做（4 级）

- `editor_undo()` / `editor_redo()`：整缓冲快照栈，撤销时把当前状态压进 redo 栈；任何新编辑清空 redo。
- 合并策略：`ED_LAST_KIND`（1=字符、2=退格、3=换行）——同类连续操作只存一次快照；光标移动把 kind 归 0，下一次编辑重新开一步。
- 日志：`[gui-shell] editor undo <剩余级数>` / `editor redo <剩余级数>` / `editor undo empty`。

### 3. Ctrl 快捷键（真实可用）

- `mb_kbd.c`：`Ctrl+字母 → base & 0x1f`（ASCII 控制码），与字母解码同一个字节；不动 shift/caps 逻辑。
- `slint_bridge.c` / `lib.rs` / `shell.rs`：ABI 增加 `mods`（SHIFT/CTRL/ALT 位，来自 `mbos_kbd_mod_state()`），外壳把 0x1A/0x19/0x13 路由到撤销/重做/保存，并在 `editor ctrl` 日志里记录（门禁据此断言「是修饰键而不是字母」）。
- 窗口内滚轮：焦点在编辑器时，一格 = 3 行视口滚动（与菜单/图片查看器同一套符号约定）。

### 4. 门禁 `gui_editor_live_acceptance.py` 重写为 **18 项**

真键盘（QMP `send-key`）走完整链路：`startgui` → 开始菜单第 4 行 → 编辑器 → 打 `xy`、回车、`abc` → **Ctrl+Z ×2 + Ctrl+Y ×1** → 打 `zz` → **11 次回车**（把光标推过 8 行视口）→ PageUp → 滚轮上滚 → F5 保存。
断言：48×64 几何日志、撤销/重做日志与级数、`editor ctrl`、光标行（PageUp 后必须落在第 5 行）、**视口滚动两次且滚轮把视口移回更上方**、`editor saved N bytes rc=0`、**宿主解析刚启动过的 FAT32 镜像逐字节比对 16 行文档（62 字节）**、屏幕仍有 1218 个亮像素。

---

## ■ 验证

**同一份 ISO、一次连续 `scripts/gui_sweep.py` 全量 32 门全绿（32/32 PASS）**：

| 门禁 | 结果 | 门禁 | 结果 |
|---|---|---|---|
| 静态渲染 static | 21/21 | 交互层 interactive | 13/13 |
| 桌面外壳 shell | 55/55 | 交换链 swap | 30/30 |
| 合成器 damage | 28/28 | C 桌面任务栏 apps_taskbar | 9/9 |
| **编辑器落盘 editorlive** | **18/18** | 开始菜单 menulaunch | 10/10 |
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
编辑器门禁自己证明了 **48×64 几何、4 级撤销/重做、PageUp 一页 8 行、滚轮把视口移回更上方**，
以及 **16 行文档（62 字节）在宿主解析的镜像里逐字节正确**。

---

## ■ 产物

| 项 | 值 |
|---|---|
| 版本 | `v0.66.0-alpha` |
| tag | `v0.66.0-alpha`（附注标签，内容见 `build/tag-notes-v0.66.0-alpha.txt`） |
| ISO | `sutureos.iso` **23,853,056 B** |
| sha256 | `dba9fd866690843ed9ef9e6fcee830bff3f862ee6d722ffb7be342fa748dfc2c` |
| 内核 | `kernel.elf` **6,622,528 B** |
| ISO 内版本串 | `0.66.0-alpha` 命中 5 次、`0.65.0-alpha` 命中 0 次 |
| 下载直链 | https://github.com/Fanqi89/SutureOS/releases/download/v0.66.0-alpha/sutureos.iso |
| 发布页 | https://github.com/Fanqi89/SutureOS/releases/tag/v0.66.0-alpha |

---

## ■ 已知限制（照实写）

1. **撤销只有 4 级**；快照是整缓冲拷贝（3 KiB/级），不做增量/差异压缩。合并粒度是「连续同类操作」，所以「打字 → 移动光标 → 继续打字」会分成两步（符合直觉，但不是按词/按行）。
2. **Ctrl 只覆盖字母**：`Ctrl+Z/Y/S` 可用（解码器把字母折成控制码），但 `Ctrl+Shift+Z`、`Ctrl+箭头`、`Alt` 组合没有语义；非字母键的 Ctrl 组合仍只按普通键处理。
3. 编辑器仍然只有 **ASCII**：CJK 能画（`char_cjk`）但键入路径只接受 0x20–0x7E，且没有输入法。
4. 缓冲上限 48 行 × 64 列是**编译期常量**；超长行在 64 列处停止插入（不折行、不报警）。
5. 保存仍固定写 `/mnt/fat/GUIEDIT.TXT`（F5 或 Ctrl+S），没有「另存为」；窗口高度固定，因此可视 8 行也是固定的（最大化不会显示更多行）。
6. 滚轮在编辑器里是**视口**滚动（3 行/格、无平滑），不移动光标；QEMU 只注入得上滚方向（v0.65 已记录），下滚方向仍由内核合成包自检覆盖。
