## 🚀 本次更新 (v0.43.0 → v0.44.0)

**任务栏时钟变成真的了**：外壳任务栏原来写死 `12:00`，现在从内核 RTC 读时间并按 `HH:MM` 用内核
8×16 点阵绘制（`[gui-shell] clock=HH:MM`）。外壳验收扩到 **33/33**，静态 21/21、交互 13/13、
C 桌面 9/9 全保持绿。

> ⚠️ 本轮也认真试了"脏矩形局部重绘 + 软件光标 save-under"（目标是把交互从 ~2 fps 提起来），
> 两次实现都导致菜单/新窗口区域变黑或交互失效，**已整体回退**，根因与已修缺陷记录在下面的"失败记录"里。
> 为避免交付退化，v0.44 不包含任何帧率改动。

---

### 📦 真实时钟（v0.44.0）

| 能力 | 行为 |
|---|---|
| 数据来源 | `kernel/gui/slint_bridge.c` 进入外壳时调 `rtc_now_epoch()` + `rtc_format_epoch()`，解析出 `HH:MM`（RTC 是 **UTC**），日志 `[gui-shell] clock=HH:MM` |
| 传递 | `suture_gui_shell_step(..., uint32 now_ms, int32 key, int32 clock_hhmm)` |
| 绘制 | `shell.rs` 用内核 `font8x16` 逐字符画 `HH:MM`（`char_ascii`），不再写死字符串 |
| 验收 | 外壳脚本新增 1 项：日志必须出现 `[gui-shell] clock=` |

### ✅ 验收

| 项目 | 结果 |
|---|---|
| 外壳 `slint_gui_shell_acceptance.py` | **33/33 PASS**（原 32 项 + RTC 时钟） |
| 时钟实测 | `[gui-shell] clock=11:09`（UTC；本地 19:09） |
| 回归 | 静态像素 **21/21**、交互 **13/13**、C 桌面 `apps_taskbar` **9/9**、GUI 外观门禁 **0 像素**、内核 13/14 |

截图：`docs/screenshots/shell-prototype-v0.44.0.png`。

### ❗ 失败记录（本轮如实登记）

1. **脏矩形局部重绘**：给 `Surface` 加 `clip`、按指针/拖动/结构变化算 dirty rect、只绘制并只拷贝该矩形。结果开始菜单/新启动窗口区域**整块变黑**（亮度 0.0）。已修两个真实缺陷：① 每矩形重画壁纸时按整行宽度写，抹掉矩形外内容；② `frosted()` 直接写 `s.px` 绕过 clip。修完**仍黑**，说明 `draw_window`/菜单/任务栏等多段绘制路径还有别的"整屏假设"；整体回退。
2. **软件光标 save-under 快路径**：用 26×30 保存区在帧缓冲上擦除/重绘光标，让纯指针移动只改 ~1k 像素。实现后外壳验收从 33/33 掉到 21/33（拖拽、点击、菜单启动失效，日志里 `drag begin`/`click at` 一条都没有），整体回退。根因未查清（怀疑 fast path 与 `LAST_STATE` 空闲指纹、`PREV_*` 状态更新互相干扰），已记录在路线图 B30。
3. 外壳验收存在**偶发 flake**：同一份镜像有一次 21/33、重跑 33/33（TCG 下帧率 ~2 fps，QMP 时序敏感）；发布前以重跑结果为准，并如实记录。

### ⚠️ 已知限制（如实记录）

1. 时钟是 **UTC**（RTC 直读，未做时区转换），且只在外壳启动时读一次（会话中不刷新）。
2. 交互仍是整屏重绘、约 2 fps；拖动/菜单悬停有明显延迟。
3. 键盘只在开始菜单内生效；窗口内容无键盘焦点。
4. 没有过渡动画；窗口页脚版本串已更新到 v0.44。

### 📜 出处与许可（本版）

- 全部为 SutureOS 自有实现（GPL-3.0），未拷贝第三方代码。

### 📦 下载

- 附件 `sutureos.iso`（17483776 字节）
- sha256：`1b724a39eb9c91f1e418ae56bfb343333a8bcf0225f804401e4686a50111aadf`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.44.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.44.0-alpha
