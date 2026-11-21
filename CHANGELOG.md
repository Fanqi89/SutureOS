# Changelog

All notable changes to SutureOS are documented here.
The version numbers follow [Semantic Versioning 2.0.0](https://semver.org/) and
every release is cut as an annotated git tag `v<version>` whose message also
describes the release. Tags are never moved or deleted, so all versions coexist
in the repository.

## [Unreleased]

### GUI 第二轮（v0.61 未发布）：桌面文本编辑器真编辑 + damage tile 网格 + 活数据遮挡修复

- **桌面文本编辑器真编辑**：`apps.rs` 新增多行缓冲区（10 行 × 38 列）与
  `editor_key/editor_move/editor_snapshot/editor_caret`；`editor_body` 改画真实缓冲区
  （行号、当前行高亮、光标块、"Ln x, Col y ・ N 行 ・ F5 保存" 状态栏）。焦点在编辑器窗口时
  按键路由到缓冲区（可打印字符、Backspace、Enter、左右上下、Home/End），**F5 经内核 VFS
  `vfs_write_file` 把缓冲区写进 `/mnt/fat/GUIEDIT.TXT`**（串口日志 `editor saved N bytes rc=0`）。
  新增 `scripts/gui_editor_live_acceptance.py` **12/12**：真 QEMU 真键盘输入 → F5 → 宿主解析
  FAT32 镜像逐字节比对（不信任客人自述），并断言输入文本在屏幕上可见。
- **修活数据穿透上层窗口**：任务管理器的 uptime/CPU%/心跳点原本在所有窗口层之后统一绘制，
  会穿到叠在上面的窗口里（实测心跳点出现在编辑器窗口 (858,342)）。改为每个窗口的活数据紧跟
  自己的层、按 z 序绘制，上层窗口自然覆盖；验收加像素断言（该处 cyan 像素必须为 0）。
- **damage 合并细化（tile 网格）**：`Damage` 改为 32×32 tile 位图（32×24）为唯一真源，
  `add()` 置位后按行扫描 + 纵向合并重建矩形表（上限 16 块，满了折叠进最后一块，只会多画不会漏画），
  替代原来"最多 6 块、满了退化成整屏包围盒"。实测（30 帧窗口）安静窗口 ~250 tiles / 4 块矩形 /
  permille 132，含菜单与拖动的窗口 permille 446；oracle 仍 0 mismatch。
  `guishell_damage_acceptance.py` 扩到 **28/28**。
  随之修正光标快路径判据：tile 粒度下斜向移动会触到 4 个 tile（4096 px），像素预算会误判，
  改为 `tile_count() <= 4`。
- **桌面 Wayland pump 钩子**：本内核 GUI 外壳跑在 shell 线程里，`startgui` 期间不能同时跑 ring-3
  Wayland 客户端，而服务端只在客户端 syscall 路径里 pump → 桌面上的 Wayland 窗口收不到指针事件。
  外壳帧循环现在每帧调用弱符号 `wl_server_desktop_pump()`（空实现由 `slint_bridge.c` 提供），
  服务端据此按真实窗口矩形把指针映射进保留 surface 并打与活路径一致的 enter/motion/leave 日志。
- **Wayland 窗口摆放与输入语义**：`wl_pointer` 的 enter/motion/leave 改为按 Wayland 窗口在屏幕上
  的矩形换算成 **surface 局部坐标**（`local = (mouse - window_origin) / buffer_scale`，窗口内才发
  enter/motion、离开发 leave）；窗口矩形优先取 Rust 外壳的 `suture_gui_wayland_rect()`（桌面运行时
  `source=shell`），终端里跑活客户端时退化为默认 (250,330,440,330)。键盘按
  `suture_gui_wayland_focused()` 门控，未聚焦时丢键并计数（`[wl] keyboard key gated ... gated=N`）。
- **keymap 真实下发**：2614 B 精简 XKB keymap 写进 memfd，通过 SCM_RIGHTS 在
  `wl_keyboard.keymap(format=1, size=2614, fd=..)` 发给客户端，客户端 map_shared 后算 FNV-1a；
  服务端、客户端、宿主三方一致（`43d6be15`）。
- **buffer 生命周期**：release 每 attach 周期只发一次；`wl_buffer.destroy` 后该 surface 不可再
  attach；`wl_shm_pool.destroy` 保留 fd（已建 buffer 仍可 attach+commit，但 create_buffer 被拒）；
  新增 `wl_server_buffer_stats()`（实测 created=2 released=4 destroyed=1 skipped=0）。
- **两个真 bug（桌面 pump 一直跑不起来的根因）**：
  - **连接槽泄漏**：`wl_server_pump()` 读到 0（对端关闭）只 break，`g_conns[i].used` 永远为 1 →
    4 槽连接表会耗尽，且"有活连接就 return"的 desktop pump 永久早退。修：`conn_release()` 在 EOF
    后（先 dispatch 完缓冲请求）释放槽，**保留 g_keep[] 里的 surface**（桌面继续显示最后一帧）。
  - **归属判据错**：本内核 ring-3 退出后 unix 对端仍不报 EOF，槽看着还活着；改为按"谁拥有输入
    设备"判定——先取窗口矩形，`source=shell` 时 shell 拥有输入、单任务内核下没有客户端能在 pump，
    桌面 pump 必须跑；只有终端路径才尊重"活连接独占两条流"。
- 验收（同一份 ISO）：静态 **21/21**、交互 **13/13**、外壳 **55/55**、交换链 **30/30**、
  合成器 **28/28**、编辑器 **12/12**、桌面 **19/19**、外观门禁 **0 像素**；Wayland 服务端 **28/28**、
  协议 **29/29**、多客户端 **40/40**、**窗口摆放/输入语义 30/30**、wlshow **9/9**、wl 窗口 **11/11**、
  wlprobe **14/14**、unix **18/18**、shm **18/18**、fdpass **21/21**。

### Wayland 进桌面窗口 + 协议补全 + 双缓冲交换链（v0.59.0 之后）

- **Wayland surface 进入桌面窗口**：图片查看器现在直接绘制内核 Wayland 服务端最后一次 commit 的
  `wl_surface`（1:1 裁剪 + accent 边框 + `Wayland surface (wl_shm)` 标注；没有 commit 时仍显示
  Logo 预览）。窗口层缓存键里混入 `wl_server_commit_count()`，客户端再 commit 会自动重栅格化。
  新增 `scripts/wl_desktop_window_acceptance.py` **11/11**（真实鼠标开菜单→打开窗口→像素统计：
  8159 个高饱和样本、4304 种颜色、边框 100% 暖色）。
- **Wayland 协议补全**：`wl_surface.frame` → 下一次 commit 发 `wl_callback.done(serial)`；
  `damage`/`damage_buffer` 累计并折进 commit 行；`wl_display.error`/`delete_id`；
  `wl_seat.get_pointer` 变成真 wl_pointer（enter/motion/button 取自内核鼠标快照）。ring-3
  `run wlclient` 扩展为两次 commit + frame 回调 + seat 事件 + 未知对象触发 error（客户端 37 项自检）。
  `scripts/wl_protocol_acceptance.py` **29/29**：宿主独立重算两帧 FNV-1a（frame1 `c7763445` 与
  `WL.BIN` 256000 B 逐字节一致；frame2 `e6601b45` 与内核第二条 commit 一致）。
- **交换链 + vsync 模拟**：`FRAME_A`/`FRAME_B` 双场景缓冲 + FRONT 指针，所有渲染路径画后台缓冲，
  present 时翻页、整帧拷贝到 framebuffer 后再画光标（screendump 不可能拍到半帧）；10 ms 闸门 +
  合并（pending 帧在 idle 分支冲刷，不丢帧）；每 30 帧打
  `[gui-shell] swap presents=N gens=G skipped=S interval_ms=M`，新增 `guishell swapstat` 输出守恒关系
  `rendered == scene + skipped`。`scripts/guishell_swap_acceptance.py` **19/19**（指针快速移动
  16.3 presents/s；桌面外观 0 像素越界）。
- 回归（同一份最终 ISO 真跑）：Wayland 29/29、28/28、9/9、wlwin 11/11、swap 19/19、桌面 **19/19**、
  外壳 **55/55**、应用内容 **25/25**、Logo **14/14**、静态 **21/21**、交互 **13/13**、C 桌面 **9/9**、
  两套外观门禁 **0 像素**、自检 **16/16**、符号 **1407 / 0 冲突**。
  产物：`kernel.elf` 6,483,096 B、`sutureos.iso` 23,574,528 B（未发布）。
- 如实限制：`skipped` 实测恒为 0（外壳循环每帧 ≥3 ticks=30 ms 节流，帧进不到 10 ms 闸门，合并机制
  已实现但正常操作不触发）；仍无真 vsync/三缓冲；`wl_pointer` 是快照不是事件流（无 leave/后续
  motion/`wl_pointer.frame`）；`wl_keyboard` 仍 inert；damage 只累计不驱动局部重绘；WL.BIN 存
  frame 1，frame 2 由宿主按"改 32×32 块"重算校验。

### GUI 收尾：三缓冲 + 60 Hz 节拍、damage 局部重绘、多客户端 Wayland 与真输入事件流、Wayland 独立窗口、视觉打磨

- **三缓冲 + 高频时基节拍**：`FRAME_A/B/C` + `DRAW/READY/PRESENTED/FREE` 状态机（每帧校验
  "呈现源必须是 READY、没有缓冲仍在 DRAW"，`buf_checks=.. buf_bad=0`）；呈现对齐 60 Hz 网格
  （`FRAME_NS=16_666_667`，时基来自 HPET `time_source_now_ns()`），统计
  presents/presented/skipped/deferred/dropped/jitter_us/latency 与间隔直方图，`guishell swapstat`
  输出全部字段，守恒式 `rendered == presented + skipped + pending`。
- **damage 驱动局部重绘**：唯一合成入口 `compose_rect()`——壁纸矩形拷贝 → 窗口层（含阴影）→
  `apps::draw_live` → 三处淡入淡出 → 焦点输入行 → 任务栏整条 → 开始菜单整块；**全屏重绘就是
  一个全覆盖 rect**，两条路径构造同源。每 ≤20 个 damage 帧 + 结构性变化跑 oracle：整屏重画到空闲
  缓冲逐像素比对（`damage: identical` / `MISMATCH at x,y`），不一致就用 oracle 帧修复
  （实测 `oracle=9.. mismatch=0`）。纯指针/空闲帧重绘占比 **permille 4–5（0.4–0.5% 屏幕）**，
  拖动重绘 **0–1 ticks（0–10 ms，旧整屏 ≈117 ms）**。
- **半帧（撕裂）检测（诚实版）**：没有扫描线中断，present 后按固定采样（96 点 + 跳过光标框）比对
  "刚呈现的缓冲"与 framebuffer，不一致记 `tear: partial present at x,y`；本轮 `tear=13,919 采样 /
  tear_bad=0`，另有验收连续 20 次在 present 过程中 screendump，与参考桌面 **0 像素差异**。
- **多客户端 Wayland**：每个连接独立保留自己最后一次 commit 的 surface（像素副本 + w/h/stride/
  scale/serial），新增 `wl_server_surface_count()/_at()`，旧 `wl_server_latest_surface()` 语义不变
  （`wlshow`/图片查看器不动）；新 ring-3 客户端 `run wltwo` 单进程开两条连接
  （160×100@2x + 96×64@1x 子 surface），导出 WLTWO.BIN/WLTWO2.BIN 供宿主逐字节比对。
- **`wl_keyboard` / `wl_pointer` 真事件流**：`kernel/drivers/mb_kbd.c` 在解码路径镜像进 64 项环
  （`mbos_kbd_poll()` 语义不变，shell/GUI 照旧消费），server pump 发
  `keymap(NO_KEYMAP)/enter/modifiers/key(evdev=sc+8, press+release)/leave`；指针改为
  enter/motion/button(两沿)/leave + `wl_pointer.frame`（v5+ 绑定才发）的持续事件流。
  实测一次打字 8 press + 9 release、8 个不同 keycode；指针 motion 3 / button 1/1 / frame 6 / leave 1。
- **`set_buffer_scale` / `wl_region` / `wl_subcompositor`/`wl_subsurface` 语义**：
  `wl_surface.set_buffer_scale(2)` 记录并导出（commit 行 `scale=2 logical=80x50`）；
  `wl_region.add/subtract` 面积累计（8000→9600→9500）+ opaque/input 面积上报；子 surface 记录
  父子与相对偏移（`pos=24,16`）。**都是语义**：不做裁剪、不做合成，报告里如实写明。
- **Wayland 独立窗口**：新窗口（accent `0x7a5cff`）"Wayland 客户端"，**F2 开关**（开始菜单保持
  4 行不变，避免移动被冻结的菜单几何与被它锚定的验收坐标）；内容按整数最近邻 1:1/2x 显示 surface、
  页脚显示 `WxH ・ scale ・ commit` 与"客户端 N #i WxH"（多客户端可观测）；无客户端时显示提示卡，
  **不回退 Logo**（"没有客户端"要看得见）。窗口层缓存键混入 commit 计数，客户端再 commit 即重栅格化。
- **视觉打磨（基线本轮重生）**：壁纸加顶部冷光晕 + 六环整数晕影（亚克力采样源同步），伪 3D 图标加
  内描边/侧棱光/底部反光，窗口加 2 px accent 身份条 + 玻璃内高光 + 22px 倒角控制钮，任务栏圆角
  按钮/圆角活跃下划线/顶部发丝高光，开始菜单圆角行。`docs/baseline/base_gui.png` 已按新外观重生，
  门禁仍然 **0 像素**越界；token 与硬约束写在 `docs/gui-visual-spec.md`。
- **修的真 bug**（现象 → 根因）：
  - 关窗后画面残留旧窗口 → 动画计数在快照之后才推进，SNAP 记成"已关闭"而画的是"还在淡出"，
    下一帧 diff 认为无需重绘；改为**快照在动画计数推进之前**拍摄。
  - 被节拍推迟的 present 丢 damage → 被下一帧取代后那份矩形从未 blit；改为 **ACCUM 累积到真正上屏**。
  - `blit_logo_region` 绕过 `Surface::clip` → 写到自己 rect 之外污染相邻像素（oracle 抓到 px=45 级差异）；
    加 `in_clip()` 判断。
  - `draw_live` 心跳点相位与活数据刷新不同帧 → 把喂给它的时钟钉在 `live_tick*1000`，既保持观感又让
    oracle 能验证该区域。
  - **`mmap(addr=NULL)` 二次映射覆盖前一次 PTE**（memfd A 24576B 与 B 64000B 落到重叠区间，透过 A 读到
    B 的数据）→ 新增 `user_map_alloc()` 单调下降游标，memfd 与匿名两条分支都从游标取窗口
    （munmap 不还窗口，作为已知限制记录）；`wltwo` 把它变成常驻断言（不重叠 + 第一次映射数据未变）。
- **验收（同一份最终 ISO 真跑）**：静态 **21/21**、交互 **13/13**、外壳 **55/55**、交换链 **30/30**、
  合成器 **23/23**、C 桌面应用/任务栏 **9/9**、桌面 **19/19**、应用内容 **25/25**、Logo **14/14**、
  Wayland 服务端 **28/28**、协议 **29/29**、多客户端 **40/40**、wlshow **9/9**、Wayland 窗口 **11/11**、
  wlprobe **14/14**、GUI 中文 4/4、图片查看器 4/4、编辑器 5/5、unix **18/18**、shm **18/18**、
  fdpass **21/21**，两套外观门禁 **0 像素**，开机自检 **16/16**，符号 **1421 全局 / 0 冲突（144 对象）**。
  产物：`kernel.elf` 6,512,328 B、`sutureos.iso` 23,631,872 B
  （sha256 `828fcd13ffe92fc2dbfe894a2da8917f9d78a6c73731e1db9eee23419aef0a81`，未发布）。
- **已知限制**：没有真 vsync 源（60 Hz 是 HPET 墙上时钟网格，present 是同步 blit，"对齐"只保证不早于
  槽边界）；`dropped` 是软件推断（错槽/被取代帧，空闲期不计）；oracle 是抽样时点（≤20 damage 帧 +
  结构性变化，非每帧）；damage 合并 ≤6 块、满了退化成包围盒（偏多但不丢）；最大化/还原 4 帧过渡仍整屏；
  3-tick 节奏下三缓冲轮转罕见（`deferred=0 carry=0`，第三缓冲主要作状态机/oracle 脚手架）；
  `wl_keyboard.keymap` 是 NO_KEYMAP（内核无 XKB 数据）；键盘第一拍可能先发一条 key 再发 enter；
  `wl_pointer` 坐标被夹进 surface（无窗口命中测试）；region/subsurface 只记录不合成；连接上限 4、
  surface 上限 320×200；`mmap` 窗口不回收。

## [v0.59.0-alpha]

### 桌面换代：`startgui` 改为 Rust 桌面外壳 + 最小 Wayland 服务端

- **`startgui` 现在启动 Rust 桌面外壳**（圆角窗口 + 轻亚克力 + 任务栏 + 开始菜单 + 伪 3D 应用图标）；
  原 C 桌面完整保留为 **`startgui-classic`**（`startgui test` 仍走它的指针自检，Rust 未链接时如实回退并打日志）。
  启动方式未变：滚动英文日志 → 系统名 banner → 黑底白字终端 → 显式输入 `startgui`。
- **视觉方向落地**（用户 2026-11-21 批准）：拟物化伪 3D 应用图标——开始菜单 22px、窗口标题栏 14px，
  纵向渐变盘 + 顶部高光 + 底部暗边 + 投影，四个应用各自 accent（蓝/青/橙/粉）；标题栏加 accent 下划线。
- **Logo 透明通道修复**：`scripts/mklogo_assets.py` 曾用 `convert('RGB')` 丢掉 alpha，机上每个 Logo
  都垫着一块白色方块；现在生成 0xAARRGGBB 并在外壳里 alpha 混合（不透明像素与 PNG 完全一致、
  透明像素 0 个纯白、透明区均值跟随背景）。
- **键盘输入队列**：一帧里从 8042 读到的多个按键不再只保留最后一个（修复快速连打丢字，实测 "hello"→"hllo"）。
- **每窗真实应用内容**：系统设置三行设置项、任务管理器实时 uptime/CPU% + 两条进度条、
  图片查看器棋盘格 + 真实 48px Logo 3× + 文件名标注、文本编辑器三行带行号代码 + 光标块。
- **最小 Wayland 服务端**：named AF_UNIX 端点（bind/connect，int 0x80 号 15/16）+ wl_display/
  wl_registry/wl_callback/wl_compositor/wl_surface/wl_shm/wl_shm_pool/wl_buffer/wl_seat 子集；
  ring-3 `run wlclient` 用 SCM_RIGHTS 交付 memfd，commit 时服务端从 memfd 读像素并算 FNV-1a；
  新增 `wlshow` 命令把最后 commit 的 wl_surface 整屏呈现（整数最近邻缩放 + accent 边框）。
- **外观基线换代**：新桌面冻结为 `docs/baseline/base_gui.png`，C 桌面基线迁到
  `docs/baseline/base_gui_classic.png`；`gui_appearance_check.py` 支持 `--profile new|classic`。
- 验收：桌面 **19/19**、外壳 **55/55**、应用内容 **25/25**、Logo **14/14**、Wayland 服务端 **28/28**、
  wlshow **9/9**、静态 **21/21**、交互 **13/13**、C 桌面 **9/9**、两套外观门禁 **0 像素**、
  自检 **16/16**、符号 **1405 / 0 冲突**。
- 产物：`kernel.elf` 6,400,672 B、`sutureos.iso` 23,410,688 B（版本号仍 `0.58.0-alpha`，未发布）。

### 新 GUI（Rust 外壳）第二轮：阴影合成、窗口内键盘焦点、真实过渡

- 修复窗口阴影变成黑色硬边（v0.46 起层缓存把阴影烘在黑色背景上，窗周实测 (0,0,0)）：
  阴影改为按距离查表的 8 层像素环，逐帧 alpha 合成到场景，层缓存只保留不透明窗体，
  圆角 AA 用所在位置的壁纸预填。验收断言：阴影带 == 壁纸 × (1-α)
  （band (85,82,196) vs 期望 (85,82,196)）、上层窗口阴影正确压暗下层窗口体
  （shaded (136,107,216) vs body (148,118,237)）。
- 新增窗口内键盘焦点与可编辑行：点击窗口取得焦点，可打印字符进入该窗口的输入行，
  `Enter` 提交（串口 `[gui-shell] input win=N text=...`）、`Backspace` 退格、
  `Tab` 在可见窗口间循环焦点（`focus cycle N`）；输入行只在焦点窗口内绘制，
  窗口层缓存保持静态。
- 最大化/还原改为真正的几何过渡：4 帧内把窗口体层最近邻采样到插值矩形
  （几何立即到目标，点击命中不受动画影响），结束后接原有 3 帧淡入；
  `scale begin` / `scale N` / `scale done` 可验收。
- 关机键 `S` → **`F10`**（外壳现在接受可打印文本，必须把 `S` 留给窗口输入）。
- 验收：外壳 **55/55**（新增阴影 2、Tab 焦点 1、过渡 3、键盘输入 3、确定性最小化 1）；
  `logo_acceptance` **12/12**（改用 F10）；静态 **21/21**、交互 **13/13**、C 桌面 **9/9**、
  外观门禁 0 像素。C 桌面与冻结的启动呈现未改动。
- 版本号推进到 `0.58.0-alpha`（未发布）。

### 新 GUI（Rust 外壳）与 Logo 资产（上一轮，未发布）

- 修复 `guishell` 关闭按钮点击路由：点击命中改用点击坐标而非当前指针，z 序改为单调计数器
  （菜单启动与聚焦同帧不再产生相同 z），关闭按钮现在稳定生效；外壳验收 **42/42**。
- 新增 `scripts/mklogo_assets.py`：把 `logo/` 尺寸阶梯编译成 `kernel/gui/logo_assets.c`
  （24px 开始按钮图标、48px 开始菜单图标、768px 关机动画图；运行时不解码 PNG）。
- `guishell` 开始按钮与开始菜单头部改用真实 Logo，逐像素验收：24px 图标 0/576 像素差异，
  48px 图标 0/2304 像素差异。
- 新增 `S` 键关机动画（v0.58.0 起改为 `F10`，因为外壳开始接受窗口文本输入）：暗化桌面 → 显示 768px Logo → 3×3 分块逆序收起 → ACPI S5 关机；
  `scripts/logo_acceptance.py` **12/12**（含 QEMU 真关机）。
- C 桌面与启动呈现不变：`gui_baseline_shot` 动态矩形外 **0 像素**；自检 **16/16**；
  `symbol-scan` 1385 符号 / 0 冲突。

## [v0.57.0-alpha]

**十二域移植收尾：补齐文件系统、USB HID、虚拟显示、音频、DHCP/DNS 与自写 BIOS 引导链等能力（全部真机 QEMU 验收）。**

### 新增

- 自写 64 位 BIOS 引导链（MBR → stage2 → long mode），与 Limine 共用同一 `kernel.elf`；`selfboot_acceptance` 25/25。
- 真实 HPET、HPET 校准 LAPIC 定时器、通用时基 `time_source`（`hpet_acceptance` 13/13、`time_source_acceptance` 10/10）。
- 文件系统：ext4 只读（44/44 + 宿主 217/217）、VimtuFS2 只读（62/62 + 173/173）、OS67 Minix-v1 只读（14/14 + 128/128）、MayFS 可写（27/27 + 42/42 + recovery 13/13）；合成 procfs（9/9）与 sysfs（9/9）。
- 存储/外设：USB HID 键盘鼠标 + 热插拔（14/14）、SB16 + 8237 DMA（23/23）、AC'97 输出音量（7/7）、HDA 输出音量（9/9）。
- 图形：virtio-gpu 2D（27/27）与 VMware SVGA II 2D（34/34），均为显式 opt-in、默认启动不触碰。
- 网络：DHCP 客户端 + DNS 解析（17/17）。
- 文档：`docs/porting-domains-report.md`、`docs/porting-final-summary.md`、`docs/porting-source-map.md`（64 个候选来源状态表）、双语 README。

### 修复

- `moos_intr` 自检长期 13/14：中断处理对异常向量重复计数；删除重复计数后开机自检 16/16。
- procfs 所有读取被拒：`VFS_O_RDONLY` 位掩码把只读打开误判为写；改为按 `mode = flags & 3` 只拒绝 WRONLY/RDWR。
- DHCP/DNS 永远超时：`net_recvfrom(MSG_DONTWAIT)` 在 `net_dgram_recv()` 之前返回 `-EAGAIN`，轮询路径不驱动 RX 环；改为自行 `net_poll()` 并有界等待。
- USB HID 描述符解析失败：EHCI qTD token 的剩余字节在 bits 30:16，且 HID report descriptor 需 `bmRequestType=0x81`。
- SB16 不发声：`0xC0` 单周期 DMA 命令必须带 format 字节，且方波累加器需 `+= 2*freq`；DMA 缓冲需独立 64 KiB 对齐段（24 位地址总线）。
- FAT32 `sync` 未真正刷盘：补 `blk_flush()`。
- mmap 失败路径可能映射物理页 0：改为回滚并返回 `-ENOMEM`；munmap 回收非 memfd 帧。

### 验收

`selfboot 25/25 · hpet 13/13（自检 16/16）· time_source 10/10 · procfs 9/9 · sysfs 9/9 · sb16 23/23 · ac97 音量 7/7 · hda 音量 9/9 · usb_hid 14/14（存储 7/7）· virtio-gpu 27/27 · vmsvga 34/34 · dhcpdns 17/17 · ext4 44/44 + 宿主 217/217 · vimtufs2 62/62 + 宿主 173/173 · os67fs 14/14 + 宿主 128/128 · mayfs 27/27 + 宿主 42/42 + recovery 13/13 · GUI 外观 0 像素 · symbol-scan 1381 符号 0 冲突`

### 已知限制

见 `docs/release-notes-v0.57.0-alpha.md` 的「已知限制」一节；要点：UEFI 启动未验证、virtio-gpu/vmsvga 仅 2D 且上限 1024×768、ext4/VimtuFS2/OS67FS 只读、MayFS 单文件上限 4095 字节、部分驱动仅 QEMU 验证。

## [v0.56.0-alpha]

**四个 Wayland 前置能力首次从 ring 3 跑通：wlprobe 探针（14/14）。**

### Added
- `user/apps/wlprobe.c`：ring-3 探针（socketpair → memfd → MAP_SHARED → SCM_RIGHTS）。
- `kernel/syscall/int80.c`：探针 ABI 号 10..14（socketpair/memfd/map_shared/send_fd/recv_fd）。
- `kernel/syscall/ring3.c`：登记 `wlprobe` blob；`scripts/wlprobe_acceptance.py` 验收。

### Verification
- `wlprobe` **14/14**；回归：unix 18/18、shm 18/18、fdpass 21/21、外壳 42/42、静态 21/21、
  交互 13/13、C 桌面 9/9、FAT32 宿主 20/20、内核 13/14。

### Limits
- 探针 ABI 而非 POSIX（无 sockaddr/bind/listen/accept/完整 msghdr）；长度打包、单次 ≤4 KiB/≤1 fd；
  描述符无 refcount；VFS fd 不可传；最小 wl server 与替换 C 桌面未开始。

## [v0.55.0-alpha]

**Wayland 第三块前置：SCM_RIGHTS（socket 上传 fd，验收 21/21）——四个内核前置至此全部落地。**

### Added
- `kernel/ipc/scm_fd.c/.h`：描述符队列 + `scmfd_selftest`。
- `kernel/syscall/syscalls.c`：`sendmsg`(46)/`recvmsg`(47)，解析/回填 SCM_RIGHTS cmsg。
- `kernel/shell/shell.c`：`fdpass` 命令；`scripts/fdpass_acceptance.py` 宿主验收。

### Verification
- `fdpass_acceptance` **21/21**；`unix_acceptance` 18/18、`shm_acceptance` 18/18；
  回归：外壳 42/42、静态 21/21、交互 13/13、C 桌面 9/9、FAT32 宿主 20/20、内核 13/14。

### Limits
- 描述符沿用编码号、无 refcount、VFS fd 不能传；sendmsg 数据/iovec/fd 数量上限；
  用户态 ftruncate(memfd) 未接线；自检在 ring 0；ring-3 夹具、最小 wl server、替换 C 桌面未开始。

## [v0.54.0-alpha]

**Wayland 第二块前置：memfd + MAP_SHARED（共享内存自检 18/18）。**

### Added
- `kernel/ipc/memfd.c/.h`：memfd 对象（号段 300+）、`memfd_resize`、`memfd_map/unmap`（共享物理页）、
  `memfd_read/write`、`memfd_selftest`。
- `kernel/syscall/mem.c`：`sys_memfd_create`(319)、`sys_mmap`(9) 的 `MAP_SHARED` + memfd 路径。
- `kernel/shell/shell.c`：`shmtest` 命令；`scripts/shm_acceptance.py` 宿主验收。

### Verification
- `shm_acceptance.py` **18/18**（两处映射双向可见、无越界）；回归：`unix_acceptance` 18/18、
  外壳 42/42、静态 21/21、交互 13/13、C 桌面 9/9、FAT32 宿主 20/20、内核 13/14。

### Limits
- 无 SCM_RIGHTS；memfd fd 未进描述符表；用户态 ftruncate 未接线；4×64 页上限；
  自检在 ring 0；最小 wl server 与 ring-3 夹具未开始。

## [v0.53.0-alpha]

**Wayland 前置第一步：AF_UNIX socketpair + poll(2)（内核自检 15 项、宿主验收 18/18）。**

### Added
- `kernel/ipc/unix_socket.c/.h`：socketpair（双向环形缓冲、EOF/EPIPE/EAGAIN 语义）与 poll 就绪查询。
- `kernel/syscall/syscalls.c`：Linux 号 `socketpair=53`、`poll=7` 注册 + fd 表接入 + 用户指针校验。
- `kernel/main/kmain.c`：启动日志 `[syscall] AF_UNIX socketpair + poll registered (2)`。
- `kernel/shell/shell.c`：`unixtest` 命令；`scripts/unix_acceptance.py` 宿主验收。

### Verification
- `unix_acceptance.py` **18/18**；回归：外壳 42/42、静态 21/21、交互 13/13、C 桌面 9/9、
  FAT32 宿主 20/20、内核 13/14、rust core PASS。

### Limits
- 无 SCM_RIGHTS / memfd / MAP_SHARED / bind-listen-accept；poll 非真阻塞；仅 AF_UNIX stream；
  无 ring-3 夹具；最小 wl server 未开始；C 桌面仍是 startgui。

## [v0.52.0-alpha]

**过渡动画补齐：开始菜单 2 帧淡入、最大化/还原 3 帧淡入；拖动底纹锚点固定（落点校准一次）；验收 42/42。**

### Added
- `rust/suture-gui/src/shell.rs`：`menu_intro`（菜单淡入 + 空闲判据）、最大化/还原复用 intro 淡入、
  `Win.ax/ay` 固定玻璃锚点、拖动结束移动到落点并重新栅格化一次。
- `scripts/slint_gui_shell_acceptance.py`：扩到 42 项。

### Verification
- 外壳 **42/42**；拖动重绘 4–9 ticks（典型 6）；窗口阶段 205 ticks/30 帧；
  回归：静态 21/21、交互 13/13、C 桌面 9/9、外观 0 像素、内核 13/14。

### Limits
- 过渡是整块叠暗（非缩放/滑动）；落点重栅格化一次（~40 ms）；层不透明；无 vsync；
  时区写死 +08:00；键盘只在菜单内；Wayland 前置与替换 C 桌面未开始。

## [v0.51.0-alpha]

**窗口层与位置解耦：拖动 = 纯 blit（5–7 ticks / 50–70 ms），窗口阶段 280→205 ticks；验收 40/40。**

### Added
- `rust/suture-gui/src/shell.rs`：`Win.sox/soy` + `frosted_at()`（磨砂采样锚点）、`draw_window_shadow()`、
  `render_window_layer()`（按 home 锚点渲染含阴影的层）、`composite_window_layer()`（行 memcpy 贴层）、
  `layer_key()`（只含 w/h/slot），松手时按新位置重新锚定。

### Verification
- 外壳 **40/40**；拖动重绘稳态 **5–7 ticks**；回归：静态 21/21、交互 13/13、C 桌面 9/9、
  外观 0 像素、内核 13/14。

### Limits
- 磨砂按 home 锚点采样（拖动中固定、落点重新锚定）；层不透明（重叠时阴影带内为变暗壁纸）；
  无 vsync；最大化/还原与菜单无动画；时区写死 +08:00；Wayland 前置与替换 C 桌面未开始。

## [v0.50.0-alpha]

**图层合成器第一刀：拖动复用窗口层 + 任务栏层缓存 + 部分呈现 + 采样校验和；拖动重绘 ≈117 ms → 60–80 ms；验收 40/40。**

### Added
- `rust/suture-gui/src/shell.rs`：`drag_reuse`（拖动中 blit 窗口层，拖动结束强制终点重绘）、
  `taskbar_key()/blit_taskbar()/save_taskbar()`（任务栏整条缓存）、拖动时只呈现旧/新矩形、
  校验和每 8 像素采样、`drag repaint` 分段日志。
- `scripts/slint_gui_shell_acceptance.py`：扩到 40 项。

### Verification
- 外壳 **40/40**；拖动重绘稳态 **6–8 ticks（60–80 ms）**，任务栏 38→20 ticks/30 帧；
  回归：静态 21/21、交互 13/13、C 桌面 9/9、外观 0 像素、内核 13/14。

### Limits
- 拖动中磨砂底纹停在起点（合成器语义，松手后校准）；无 vsync，仍有滞后；任务栏缓存键未含标题；
  最大化/还原与菜单无动画；时区写死 +08:00；Wayland 前置与替换 C 桌面未开始。

## [v0.49.0-alpha]

**关闭/最小化 2 帧淡出（延迟执行）+ Slint 路线定稿（长期自写渲染器）；验收 38/38。**

### Added
- `rust/suture-gui/src/shell.rs`：`begin_act()` + 关闭/最小化前 2 帧淡出，动作延迟到动画结束后执行
  （`anim minimize/close` → `anim done`），动画期间不跳过空闲帧。
- `docs/slint-route-decision.md`：Slint 卡点证据、软浮点代价、自写渲染器达标数据与重新评估条件。
- `scripts/slint_gui_shell_acceptance.py`：扩到 38 项。

### Verification
- 外壳 **38/38**；`profile/30 ... win=282 bar=28`（未回退）；
  回归：静态 21/21、交互 13/13、C 桌面 9/9、外观 0 像素、内核 13/14。

### Limits
- 淡出为整块压暗；最大化/还原与菜单无动画；拖动仍重建窗口缓存（图层合成器 B36 待做）；
  时区写死 +08:00；圆角整数查表；键盘只在菜单内。

## [v0.48.0-alpha]

**窗口打开 3 帧淡入 + 任务栏本地时钟（UTC+08:00，每分钟刷新）；验收 37/37。**

### Added
- `rust/suture-gui/src/shell.rs`：`intro_frames/intro_idx` + 窗口打开淡入（淡入期间不跳过空闲帧）。
- `kernel/gui/slint_bridge.c`：`clock_local_from_utc()`（固定 +08:00）、启动日志
  `clock utc=.. local=.. (+08:00)`、每 6000 tick 刷新 `clock update local=..`。
- `scripts/slint_gui_shell_acceptance.py`：扩到 37 项。

### Verification
- 外壳 **37/37**；`profile/30 ... win=213`（v0.47 280，未回退）；
  回归：静态 21/21、交互 13/13、C 桌面 9/9、外观 0 像素、内核 13/14。

### Limits
- 只有打开窗口有淡入；关闭/最小化/最大化无动画；淡入是整块压暗；时区写死 +08:00；
  拖动仍重建窗口缓存（图层合成器未开始）。

## [v0.47.0-alpha]

**掐掉圆角 AA 的软浮点 `sqrt`（覆盖率查表）：frosted 165→17 ticks/4 次绘制，窗口阶段 916→280 ticks/30 帧，整屏重绘 ≈330→117 ms；验收 35/35。**

### Fixed
- `rust/suture-gui/src/shell.rs`：`CORNER_COV[16][16]` 预计算圆角覆盖率，圆角带不再 per-pixel 调用
  `fsqrt`（无 SSE 目标上每次都是软浮点调用）；内部区域纯整数循环。

### Added
- 分段计时日志 `[gui-shell] wsplit shadow=/frosted=/chrome= ticks (x4)` 常驻，供后续性能对比。

### Verification
- 外壳 **35/35**；`frosted=16`（v0.46 `173`）、`win=280`（v0.46 `916`）；
  回归：静态 21/21、交互 13/13、C 桌面 9/9、外观 0 像素、内核 13/14。

### Limits
- 圆角按整数像素中心查表（1 px 级精度）；整屏重绘仍 ~117 ms（下一步：图层合成器）；
  默认两窗口重叠处不缓存；时钟 UTC 只读一次；键盘只在菜单内；无过渡动画。

## [v0.46.0-alpha]

**窗口层缓存（几何键，含阴影）：窗口阶段 1416→916 ticks/30 帧（−35%）；修掉循环帧数上限与一条靠错因通过的断言；验收 35/35。**

### Added
- `rust/suture-gui/src/shell.rs`：`win_key()`/`blit_window_cache()`/`save_window_cache()`；命中即 blit，
  否则重画回填；profile 增加 `full/hit/miss`。
- `scripts/slint_gui_shell_acceptance.py`：扩到 35 项。

### Fixed
- 外壳循环安全网 `frames > 2400` → `20000` + 15 分钟上限（快帧路径会让旧上限在验收中途触发）。
- `closed window is gone` 断言改为与启动前基线比较（旧 `<85` 实为壁纸亮度）。

### Verification
- 外壳 **35/35**；`profile/30: full=30 hit=27 miss=27 win=916 bar=97`（v0.45 win=1416）；
  回归：静态 21/21、交互 13/13、C 桌面 9/9、外观 0 像素、内核 13/14。

### Limits
- 默认两窗口互相重叠 → 重叠不缓存；拖动/最大化每帧重建；整屏重绘 ~330 ms（TCG 混合 ≈80 ns/px）。
  下一步：图层合成器。

## [v0.45.0-alpha]

**指针移动快路径：光标从场景缓存分离、直接合成进 framebuffer；指针移动 2–4 → 13–14 fps；验收 34/34。**

### Added
- `rust/suture-gui/src/shell.rs`：`FRAME` 只存不含光标的场景；`blit_rect()`/`draw_cursor()`/`hover_code()`；
  纯指针移动走 fast path（恢复旧/新光标矩形 + 画箭头，日志 `cursor: fast path frames=N`）。
- `scripts/slint_gui_shell_acceptance.py`：扩到 34 项。

### Verification
- 外壳 **34/34**；串口心跳 30 帧 2150–2310ms → **13.0–14.0 fps**；回归：静态 21/21、交互 13/13、
  C 桌面 9/9、外观 0 像素、内核 13/14。

### Limits
- 快路径只覆盖纯指针移动；悬停切换/拖动/窗口开关仍整屏重绘 ~2 fps；光标形状固定；
  场景缓存 1024x768；时钟 UTC 只读一次；键盘只在菜单内。

## [v0.44.0-alpha]

**任务栏真实 RTC 时钟（UTC，内核点阵绘制；原写死 12:00）；外壳验收 33/33。**

### Added
- `kernel/gui/slint_bridge.c`：从 `rtc_now_epoch()`/`rtc_format_epoch()` 解析 `HH:MM`，日志
  `[gui-shell] clock=HH:MM`，经新参数 `clock_hhmm` 传给 Rust。
- `rust/suture-gui/src/shell.rs`：用 `font8x16` 逐字符绘制真实时间；页脚版本 v0.44。
- `scripts/slint_gui_shell_acceptance.py`：扩到 33 项。

### Verification
- 外壳 **33/33**（clock=11:09 UTC）；回归：静态 21/21、交互 13/13、C 桌面 9/9、外观 0 像素、内核 13/14。

### Limits
- 时钟 UTC、只读一次；交互仍整屏重绘 ~2 fps；键盘只在菜单内；无过渡动画。
- **失败并回退**：脏矩形局部重绘（菜单区域变黑）、软件光标快路径（拖拽/点击失效）——根因见路线图 B30。

## [v0.43.0-alpha]

**开始菜单可用键盘：↑/↓ 选择、Enter 启动（与鼠标共用 launch 路径）；外壳验收 32/32。**

### Added
- `kernel/gui/slint_bridge.c`：shell 循环把最后一个 make-code 按键传入 Rust（`shell_step(..., now_ms, key)`）。
- `rust/suture-gui/src/shell.rs`：菜单键盘导航（↑0x48/↓0x50/Enter0x1C，循环选择、`menu sel N`/`menu enter N`），
  `launch_window()` 统一鼠标与键盘启动路径，选中项独立底色。
- `scripts/slint_gui_shell_acceptance.py`：扩到 32 项。

### Verification
- 外壳 **32/32**（menu sel 1/2、menu enter 2 + launch 2 + 窗口 124.6）；回归：静态 21/21、交互 13/13、
  C 桌面 9/9、外观 0 像素。

### Limits
- 键盘只在菜单内；一次只传最后一个按键；无过渡动画；**脏矩形局部重绘尝试失败已回退**（菜单区域变黑），
  交互帧率仍约 2 fps。

## [v0.42.0-alpha]

**桌面外壳可用：4 个窗口、开始菜单启动应用、关闭按钮、空闲跳帧；验收 30/30。**

### Added
- `rust/suture-gui/src/shell.rs`：窗口 2→4；菜单行启动/恢复/聚焦窗口；关闭按钮（任务栏按钮消失、焦点移交）；
  z 序排序；空闲跳帧（状态指纹不变即跳过整帧）；页脚版本 v0.42。
- `scripts/slint_gui_shell_acceptance.py`：扩到 30 项（菜单启动、关闭、空闲跳帧、取消最大化）。

### Verification
- 外壳 **30/30**（启动窗口亮度 124.6、关闭后 0.0、idle 跳帧日志、双击最大化/还原、ESC 退出、无 PANIC）；
  回归：静态 21/21、交互 13/13、C 桌面 9/9、外观 0 像素、内核 13/14。

### Limits
- 交互时整屏重绘（~2 fps，脏矩形未做）；菜单启动窗口内容占位；关闭无确认；无过渡动画。

## [v0.41.0-alpha]

**新渲染器接管桌面外壳：两个窗口 + 任务栏（开始/窗口按钮/时钟）+ 开始菜单；拖动、最小化/恢复、
双击最大化全部可用；验收 23/23。**

### Added
- `rust/suture-gui/src/shell.rs`：窗口状态机、任务栏命中、开始菜单、拖动与双击判定；窗口/任务栏/菜单
  用 256x192 模糊壁纸做真磨砂（整数坐标映射）；双缓冲呈现；整数 `rect_fill` 快路径。
- `kernel/gui/slint_bridge.c`：`slint_gui_run_shell()`；shell 命令 `guishell`。
- `scripts/slint_gui_shell_acceptance.py`（23 项，真 QMP 鼠标/键盘 + 像素断言）。

### Verification
- 外壳 **23/23**（拖动/最小化/恢复/双击最大化/开始菜单/退出）；回归：静态 21/21、交互 13/13、
  C 桌面 9/9、外观门禁 0 像素、内核 13/14；帧时间 1.9s → ~0.5s（TCG）。

### Limits
- 约 2 fps、无脏矩形；窗口内容占位；菜单项不可点；无过渡动画；不与 C 桌面共享状态。

## [v0.40.0-alpha]

**新渲染器活了：鼠标悬停高亮、点击涟漪、箭头光标、面板淡入；交互验收 13/13。**

### Added
- `kernel/gui/slint_bridge.c`：`slint_gui_run_interactive()`（轮询 PS/2 鼠标/键盘、每帧一帧渲染、30fps
  有界自旋、ESC 退出、帧数上限）；`guislint` 画完静态帧后进入交互循环。
- `rust/suture-gui/src/lib.rs`：`suture_gui_interactive_step()`、场景缓存（3 MiB .bss，一次渲染多帧复用）、
  `suture_gui_cache_seed()`；动效（淡入/悬停高亮+蓝色指示条/点击涟漪）。
- `swrast.rs`：`cursor()` 箭头光标。
- `scripts/slint_gui_interactive_acceptance.py`：真 QMP 鼠标/键盘 + 像素断言（13 项）。

### Verification
- 交互 **13/13**（hover row 0/1、click at、涟漪 6400px、光标 528px、ESC 退出、无 PANIC）；
  静态像素 **21/21**；C 桌面 9/9、外观门禁 0 像素、内核 13/14。

### Limits
- 仅两行列表可交互；无 vsync/双缓冲（可能撕裂）；场景缓存限 1024x768；无键盘焦点；仍是 prototype。

## [v0.39.0-alpha]

**轻亚克力：面板/卡片显示真正被模糊的背景（可分离 box blur），验收 21/21。**

### Added
- `rust/suture-gui/src/swrast.rs`：`acrylic_blur()` 两遍可分离 box blur，临时缓冲只覆盖模糊矩形（≈0.85 MiB .bss）；
  通道分别累加再打包（打包值求和的进位会造成通道串扰，第一版因此出现彩色条带）。
- 场景：面板 r=18、卡片 r=14 先模糊背景再叠半透明填充；模糊只写面板内部（外部只读），投影保持不变。

### Verification
- `scripts/slint_gui_acceptance.py` **21/21**：面板内背景硬边最大跳变 2、面板外 177、面板叠色对比；
  原 18 项（圆角/半透明/假阴影/伪 3D/5 项字形）保持通过；C 桌面 9/9、外观门禁 0 像素、内核 13/14。

### Limits
- 静态单帧模糊（无动态背景重算）；半径固定、无分层/噪点；模糊上限 1024x480 硬编码；仍静态样板帧。

## [v0.38.0-alpha]

**新渲染器接上文字：中文（GB2312 16×16）与英文（8×16）驱动自内核既有字体资产，
字形逐像素验收 18/18。**

### Added
- `rust/suture-gui/src/swrast.rs`：`text()`（UTF-8 解码、逐字符取字形、按 scale 最近邻放大、alpha 合成）。
- `kernel/gui/slint_bridge.c/.h`：`slint_bridge_ascii/cjk/cjk_ready` 把 `font8x16` 与 cjk16 位图交给 Rust，
  不做字体复制。
- 样板场景文字：标题/副标题/两行列表/卡片两行；移除与文字重叠的占位条。

### Verification
- `scripts/slint_gui_acceptance.py` **18/18**：设 hit=232、圆 hit=83、账 hit=74、个 hit=25（miss 全 0），
  原 13 项像素检查保持通过；C 桌面 9/9、外观门禁 0 像素、内核 13/14。

### Limits
- 仅 GB2312 + ASCII 32..126；最近邻放大；无换行/裁剪/排版；仍静态样板帧。

## [v0.37.0-alpha]

**GUI 重做第一步：新风格第一帧原型合入主仓（Rust 软件光栅器）+ `--gc-sections` 减重。**
`guislint` 把一张"圆角 + 轻亚克力 + 微透叠层 + Fluent 底 + 拟物化伪 3D 图标"的静态样板画进
framebuffer，像素断言 13/13；C 桌面与启动呈现零改动。

### Added
- `rust/suture-gui/`（no_std staticlib, `x86_64-unknown-none`）：`swrast.rs` 软光栅器（圆角/渐变/alpha/
  4 层假阴影/伪 3D 高光）+ 平台适配 `lib.rs`。
- `kernel/gui/slint_bridge.c/.h` + shell `guislint` 命令（画一帧并保持，ESC/10s 返回终端）。
- `scripts/build.py`：417 个 Rust 对象提取 + `@link.rsp` + `--gc-sections`（4,956,200 → 3,364,744 B）。
- `scripts/slint_gui_acceptance.py`（13 项）、`docs/screenshots/slint-prototype-v0.37.0.png`。

### Verification
- slint_gui **13/13**（圆角 |d| 69/67/73/107、半透明 |d| 196、假阴影 68.3→89.3→102.0、图标高光 223.4）；
  C 桌面 apps_taskbar 9/9、外观门禁 0 像素、内核 13/14、`[rust] core` PASS、FAT32 21/21。

### Limits
- 手写光栅器而非 Slint（Slint render 卡死/Text panic，`--features slint` 可复现）；无文字/动效/输入；
  无真模糊；只支持 32bpp；Rust GUI 仍 ring 0；Wayland 前置未做。

## [v0.36.0-alpha]

**系统升级（只做覆盖）+ Rust 系统层第一步。**

### Added
- `kernel/install/installer.c`：`installer_upgrade()` —— 按内容找装好的系统卷（`/SUTUREOS/INSTALLED`），
  从 ISO 覆盖 `/kernel.elf`、`/limine-bios.sys`、重新生成 `/limine.conf`，重刷 MBR + Limine stage2，
  写 `/SUTUREOS/VERSION`；`/etc`、`/home`、`/root`、`/SUTUREOS/PERSIST` 一律不动；复用 bulk 写模式加速。
- `shell.c`：`upgrade` 命令（root）。
- `rust/suture-core`（no_std staticlib, `x86_64-unknown-none`）+ `kernel/rust/probe.c`（weak 回退 + 探针）
  + `scripts/build.py` 提取 ar 成员链接（GNU 长名表 `//` + `/NNN`）。
- `scripts/upgrade_acceptance.py`（四阶段，11 项）。

### Verification
- upgrade **11/11**：装 v0.35 → 升级到 v0.36 → 宿主 `/KERNEL.ELF` 与 `build/kernel.elf` 逐字节相同、
  PERSIST marker 保留 → 只挂盘启动 banner `0.36.0-alpha` 且 marker 仍在、无 PANIC。
- Rust 核心：`[rust] core v10000 linked, selftest=2171600cbe2b2b8c expected=... PASS`。
- 回归：内核 13/14、FAT32 21/21、apps_taskbar 9/9、GUI 外观门禁 0 像素。

### Limits
- 覆盖式升级（无回滚/签名/版本校验）；`/etc` 模板不更新；仅 BIOS/MBR+Limine；需 ISO 启动；
  Rust 核心只验证链接与 ABI（Slint/Smithay 未接入）；既有 ATA 偶发超时（重跑即过）。

## [v0.35.0-alpha]

**数据持久化：安装到硬盘后 `/etc`、`/home`、`/root` 双向镜像到系统卷 `/SUTUREOS/PERSIST`，
开机恢复、sync/shutdown/reboot/退出 GUI 时落盘，重启不再丢数据。三阶段验收 32/32。**

### Added
- `kernel/fs/persist.c/.h`：`persist_detect/restore/flush`，VFS 递归双向镜像（有界），flush 末尾 sync 系统卷。
- `kernel/install/installer.c`：安装时建 PERSIST 骨架并把新卷挂 `/mnt/sys` + `persist_detect()`（同一会话即生效）。
- `kernel/main/kmain.c`：开机优先从 PERSIST 恢复，旧卷退回 `/mnt/sys/etc` 播种；重建 /home、/root、/home/<user>。
- `kernel/shell/shell.c`：`sync`/`shutdown`/`reboot`/退出 GUI 前 flush；新增 `persist [flush]` 命令。
- `scripts/persistence_acceptance.py`（32 项，三阶段）、`docs/PERSISTENCE.md`。

### Verification
- persistence **32/32**（含宿主解析 PERSIST 文件 + 重启后 `su bob` 成功）；安装器 56/56；内核 13/14；
  FAT32 21/21；apps_taskbar 9/9；多用户 54/54；GUI 外观门禁 0 像素；启动呈现前 297 行逐字节相同。

### Limits
- 只复制不删除同步；有界（深≤6/目录≤64/次≤256/文件≤128KiB）；无属主元数据；非原子写；
  只镜像首个系统卷；每次全量重写；既有 ATA 偶发 flush timeout。

## [v0.34.0-alpha]

**桌面外壳第一批：应用注册表（开始菜单运行时生成）+ 固定任务栏应用 + 窗口最小化/最大化完整可用
（点任务栏按钮在还原↔最小化间切换、最大化真正改几何、m/n 快捷键）。**

### Added
- `kernel/gui/apps.c`：`g_app_registry[]`（app_id/label/pinned）+ `gui_app_registry()`。
- `kernel/gui/gui.c`：`gui_start_menu_build()` 从注册表生成菜单（运行时项数决定高度与命中）；
  任务栏改为「开始 → 固定应用（≤4）→ 运行窗口（≤6）→ 时钟/用户/FPS」，`gui_taskbar_layout()` 统一几何；
  固定按钮点击 `gui_activate_app()`；窗口按钮点击在还原↔最小化间切换。
- `gui_flip_window()` 真正实现最大化/还原（`norm_x/y/w/h` → 1024×728）；双击标题栏、标题栏按钮、`m` 共用。
- 快捷键 `m`（最大化/还原）、`n`（最小化焦点窗口）。
- `scripts/apps_taskbar_acceptance.py`（9/9）。

### Verification
- apps_taskbar **9/9**、外观门禁 0 像素（基线按要求更新）、图片查看器 PASS、编辑器 5/5、
  切换用户 13/13、任务栏旧用例 7/7、内核 13/14、FAT32 21/21。

### Limits
- 固定 ≤4、窗口按钮 ≤6、标题截断；无任务栏右键菜单；最大化纯几何；菜单最多 18 项无分页；
  GUI 大改按用户要求暂缓。

## [v0.33.0-alpha]

**ISO 变成真正的系统安装盘：`install` 终端向导真实写盘（MBR + Limine BIOS 引导 + FAT32 + 系统文件），
装完只用硬盘即可启动，安装时设的密码/用户重启后仍有效。三阶段宿主侧验收 56/56。
顺带修掉既有 FAT32「目录项首簇丢失高字节」的 bug。**

### Added
- `kernel/install/installer.c/.h`：终端安装向导（选盘/`yes` 二次确认/root 密码可空/可选普通用户）+ 真实写盘。
- `kernel/fs/fatfs_format.c`：最小 FAT32 格式化器（引导扇区/备份/FSInfo/双 FAT/根簇清零/读回校验）。
- `kernel/fs/fatfs.c`：分区偏移 `start_lba`、`fatfs_mount_auto()`（超软盘优先再扫 MBR 0x0B/0x0C/0x0E）、
  安装写盘加速（FAT 链游标/整簇写/bulk 模式）。
- `kernel/main/kmain.c`：`/SUTUREOS/INSTALLED` 标记 → 同卷挂 `/mnt/sys` 并播种 `/etc`、重跑 userdb/sudo。
- `kernel/shell/shell.c`：`install` 命令（root）。
- `scripts/installer_acceptance.py`（三阶段）：ISO 安装 → 宿主解析镜像 → 无 ISO 从硬盘启动 + `su alice`。
- `docs/INSTALLER.md`：磁盘布局、shadow 空密码语义、`/etc` 播种、限制。

### Fixed
- FAT32 目录项首簇只解析 `ent[26]`（丢 `ent[27]<<8`）→ 首簇 ≥256 的文件读错数据；装真系统才暴露
  （`/etc/passwd` 读成内核机器码）。改为 4 字节解析。

### Verification
- 安装器 **56/56**（MBR/分区/FAT32 几何/引导区与 `limine-bios-hdd.h` 逐字节/kernel sha256/limine.conf/
  `/etc` 哈希 `hashlib.pbkdf2_hmac` 重验/`/SUTUREOS/INSTALLED`）；空密码路径 `root:!:0` 亦验。
- 阶段 3 只挂硬盘启动：`[sys] installed system volume ... seeded 5 file(s)` → `su alice` 成功、`whoami`=alice。
- 回归：内核 13/14、FAT32 21/21、多用户 54/54、GUI 切换 13/13、任务栏 7/7、图查看器/编辑器 PASS、
  外观门禁 0 像素、启动呈现前 286 行逐字节相同。

### Limits
- 仅 BIOS/MBR + Limine（无 UEFI/GRUB）；单 FAT32 分区；无 LUKS/LVM/swap/包选择/网络安装；
  最小盘 35 MiB；安装 60–90 s；终端向导；无日志落盘/进度百分比；多系统共存未做；
  装到已有数据的盘会覆盖分区表（默认不选第一块盘 + `yes` 确认）。

## [v0.32.0-alpha]

**多用户：`useradd`/`userdel`/`users`/`passwd -l/-u/-d`/`login`/`su -` + GUI Switch User 对话框；
`/etc/shadow` 密码三态（哈希/空=免密/`!`=锁定）有唯一判定入口；非 root 写 `/etc`、`/root` 被拒绝。
验收 54/54（shell）+ 13/13（GUI 切换），宿主用 hashlib 重算 PBKDF2 验证哈希。**

### Added
- `kernel/security/login.c/.h`：统一认证/切换引擎（失败不改身份、清 sudo 票据、刷新提示符、
  `su -` 切 home、root 免密）；`login`/`su` 走同一实现。
- `kernel/security/userdb.c/.h`：add/remove/save（三文件重写）、uid≥1000、锁定/免密/清密码、
  密码状态 API、组表访问器。
- `kernel/shell/shell.c`：useradd/userdel/users/passwd/login 命令；`/etc`、`/root` 写保护。
- `kernel/gui/apps.c`：懒创建 Switch User 对话框；开始菜单 8 项；任务栏用户改用 `login_current_user()`。
- `scripts/multiuser_acceptance.py`、`scripts/switch_user_acceptance.py`、`docs/multiuser.md`。

### Verification
- multiuser **54/54**、switch user **13/13**、FAT32 宿主 **21/21**、sudotest **23/23**、
  任务栏 7/7、图片查看器 PASS、编辑器 5/5、内核 13/14。

### Limits
- 无 PAM/login.defs/groupadd/usermod；userdel 不删 home；上限 16；盐非 CSPRNG；
  `/etc` 保护是 shell 策略层；GUI 对话框仅键盘；开始菜单 7→8 项（GUI 测试几何已同步）。

## [v0.31.0-alpha]

**任务栏增强 + 快捷键（用户 2026-11-03 要求的界面更新）：每个窗口一个任务栏按钮（点击置顶/聚焦、
最小化可点回）、RTC 实时时钟、当前用户显示；Windows/Super 键开关开始菜单，ESC 先关菜单。
外观基线按用户要求更新（重新生成 base_gui.png），风格仍全部是 SutureOS 自绘。**

### Added
- `kernel/gui/gui.c`：任务栏窗口按钮（`gui_taskbar_windows`/`gui_taskbar_btn_rect`）、
  RTC 时钟、当前用户（`cred_euid` → `userdb_by_uid`）、任务栏点击聚焦。
- Windows/Super 键（E0 5B/5C，只认 make code）切换开始菜单；ESC 在菜单打开时只关菜单。
- `scripts/taskbar_acceptance.py`（7/7）；`docs/baseline/base_gui.png` 按用户要求重新生成；
  外观门禁新增任务栏 live 区域。

### Verification
- 任务栏渲染（按钮/时钟/用户/FPS）+ Win 键开关菜单 + 点击按钮聚焦 → 7/7 PASS；
  外观门禁动态矩形外 0 像素；图片查看器/GUI 中文/文本编辑器回归全 PASS。

### Limits
- 最多 6 个按钮、标题截断 14 字符；无右键菜单/托盘/显示桌面；时钟只有 HH:MM。

## [v0.30.0-alpha]

**SquashFS 支持多卷：FAT32 / ext2 / squashfs 三种真实文件系统全部做到一次启动多卷同时挂载。
本批收尾跑了全套回归门禁。启动呈现与 GUI 外观未变。**

### Added
- `kernel/fs/squashfs.c/.h`：`g_squashfs_fs`/`g_squashfs_mount` → `[4]` 数组 + 当前卷指针；
  `squashfs_select_volume/current_volume/volume_count/unmount_drive`；mount 返回卷槽号；
  句柄带 `vol`（所有认领点）。
- `kernel/fs/squashfs_vfs.c`：挂载点表 `g_squash_mnts[4]`，每操作选卷，unmount 按挂载点定位。
- `scripts/multivol_squashfs_acceptance.py`：双卷 + 宿主双镜像 83/83。

### Verification (full sweep on this ISO)
- 内核 13/14；FAT32 宿主 21/21 + 四自检；EXT2 多卷 12/12；SquashFS 多卷 7/7；
  USB 只读 5/5 + 写 8/8；HDA WAV 6/6；终端 CJK 10/10；GUI CJK 4/4；wget 8/8；
  文本工具 7/7；编辑器 5/5；sync 6/6；GUI 外观门禁 0 像素。

### Limits
- 卷槽 4、挂载点不可嵌套、句柄表 8+8；squashfs 只读（无 xz/zstd/xattr/>4KiB 块）；
  第二卷需在 AHCI/NVMe/VirtIO；USB 热插拔/HID 与 virtio-gpu 增强仍待做。

## [v0.29.0-alpha]

**EXT2 支持多卷（两个卷同时挂载、独立读写）；同时修掉上一轮暴露的跨卷写入 bug：
O_CREAT 写句柄漏记所属卷，导致写入落到第一个卷并覆盖其数据块。宿主双镜像验证
"卷 2 拿到写入、卷 1 一字未变"。启动呈现与 GUI 外观未变。**

### Fixed
- `kernel/fs/ext2.c`：所有句柄认领点（只读 open / O_CREAT 写 open / opendir）都记录
  `f->vol = g_cur_ext2_vol`——缺失的那一处让所有写操作都切到卷 0，串卷覆盖卷 0 的数据。

### Added
- `kernel/fs/ext2.c/.h`：`g_ext2_fss[4]` + 当前卷指针；`ext2_select_volume / current_volume /
  volume_count / unmount_drive`；`ext2_mount`、`ext2_mount_ramdisk` 返回卷槽号；句柄带 vol。
- `kernel/fs/ext2_vfs.c`：挂载点表 `g_ext2_mnts[4]` + 每操作选卷；unmount 按挂载点定位；
  VFS 写自检改用挂载点表。
- `scripts/multivol_ext2_acceptance.py`：双 ext2 卷 + 交叉写 + 宿主双镜像逐字节（含"卷 1 不得含卷 2 文本"断言）。

### Verification
- `mount ext2 4 /mnt/e2`（AHCI drive 4）→ 两卷独立 ls/cat → 写卷 2 + umount →
  宿主卷 1 写验证器 **40/40**、卷 2 fixture **35/35**、卷 2 变化 48 字节、卷 1 无卷 2 文本
  → **12/12 PASS**；内核 13/14、FAT32 宿主 21/21、GUI 外观门禁 0 像素。

### Limits
- 卷槽 4；挂载点不可嵌套；句柄表 8+8；仅 ext2；squashfs 仍单卷；第二卷需在 AHCI/NVMe/VirtIO 上。

## [v0.28.0-alpha]

**新增 `sync`：写盘不再必须 umount（FAT32 FSInfo / ext2 计数是延迟写的）。验收刻意不卸载，
宿主解析原始镜像逐字节确认。启动呈现与 GUI 外观未变。**

### Added
- `kernel/shell/shell.c`：`shell_cmd_sync()` —— 遍历 `vfs_list_mounts()` 逐个 `vfs_sync()`，
  逐卷报告结果与汇总，失败返回非 0。
- `scripts/sync_acceptance.py`：write → sync → cat（不 umount）+ 宿主侧 FAT32 逐字节校验。

### Not shipped (honest)
- ext2 多卷尝试：结构改动可跑通（两卷同挂、各自读写），但宿主写验证器在卷 1 报
  `artifact content byte-exact` 失败，怀疑多卷切换下写路径串卷；根因未查明，**已整体回退**，
  记录在 docs/porting-roadmap.md B12。

### Verification
- guest `sync: /mnt/fat (fat32) ok` + `sync: 1 ok, 0 failed`；宿主 `SYNC.TXT` 内容逐字节一致
  → **6/6 PASS**；内核 13/14、FAT32 宿主 21/21、GUI 外观门禁 0 像素。

## [v0.27.0-alpha]

**Intel HDA 音频（B9）：控制器 + CORB/RIRB verb + codec 枚举 + 音频输出路径 + 环形 BDL 播放，
QEMU 里真实出声；宿主侧解析 WAV 证明 48 kHz/2ch/16bit、1.002 s、0.5000 FS、440.01 Hz。
启动呈现与 GUI 外观未变。**

### Added
- `kernel/drivers/hda.c`（1236 行）+ `hda.h`：PCI 探测 / BAR0 MMIO / GCTL 复位 / CORB-RIRB DMA verb /
  codec-AFG-widget 枚举 / 输出引脚→转换器路径 DFS / amp+EAPD+PIN_OUT_ENABLE / SD0 环形 BDL 播放 /
  正弦合成；骨架 PlantOS（MIT），改动逐条记在文件头。
- `scripts/verify_hda_wav.py`：宿主侧 WAV 校验（采样率/声道/位深/时长/峰值/频率）。
- `kmain.c` 接入（无设备只多一行日志）；shell 新增 `hdatest [freq] [ms]`。

### Fixed / learned (QEMU 实测)
- CORB 第一条命令必须写在索引 1（复位后 WP=0，写回 0 无命令）。
- RINTCNT 是每批响应数；必须 RINTCNT=1 + RIRB 响应状态位 + 每条响应 W1C 清 RIRBSTS。
- SD_CTL 的 RUN 是 bit1(0x02)；stream tag 在 bits23:20。
- GET_PARAMETER id 与输出 SD0 基址（0x100）按规范取值。

### Verification
- 有 HDA：codec 枚举 → 播放 → `selftest: PASS played 48000 frames @48000 Hz`；
  宿主 WAV **6/6 PASS**（48000/2ch/16bit/1.002 s/0.5 FS/440.01 Hz）。
- 无 HDA：模板差异仅 `+[hda] no controller`；13/14 自检、FAT32 21/21、外观门禁 0 像素；警告零新增。

### Limits
- 仅 QEMU 验证（真机未测）；单 codec/单输出 pin；无采集、无音量控件、无中断；
  固定 48 kHz/16-bit/立体声；单次 ≤1 s；WAV 含 600 ms drain 静音尾巴；有 HDA 时开机 +1~2 s。

## [v0.26.0-alpha]

**命令行补齐五个文本工具：`wc` / `head` / `tail` / `grep` / `stat`（全部走公开 VFS，四种文件系统
一致可用）；验收用 shell 自己造多行文件再逐条断言，宿主侧解析 FAT32 镜像逐字节复核。
启动呈现与 GUI 外观未变。**

### Added
- `kernel/shell/shell.c`：`wc`（行/词/字节流式统计）、`head`（前 n 行）、`tail`（最后 n 行，
  偏移窗口 + vfs_seek 回读）、`grep`（字面子串 + 行号 + 命中计数）、`stat`（类型/大小）。
- `scripts/textutils_acceptance.py`：QMP 注入 10 条命令 + 串口断言 + 宿主侧 FAT32 逐字节校验。

### Verification
- `wc` → `5 10 53`；`head 2` 只前两行；`tail 2` 只后两行；`grep alpha` → 2 处带行号；
  `stat` → regular file / 53 bytes → **7/7 PASS**；宿主 `TXT.TXT` 逐字节一致。
- 回归：内核 13/14、FAT32 宿主 21/21、无 PANIC、GUI 外观门禁 0 像素。

### Limits
- grep 非正则、区分大小写、无 -i/-v；单行 >512 字节截断；tail 窗口 64 行；无管道；tail 扫两遍。

## [v0.25.0-alpha]

**第五个桌面应用：Text Editor —— 开始菜单打开、真实键盘输入、回车保存到 /mnt/fat/EDIT.TXT，
宿主解析 FAT32 镜像逐字节验证。启动呈现与默认桌面外观未变。**

### Added
- `kernel/gui/apps.c`：`APP_ID_EDT=7` 文本编辑器（加载/插入/退格/换行保存/光标/状态行，
  1 KiB 缓冲，SutureOS 自绘）；开始菜单新增 `Text Editor`（懒创建，默认桌面不变）。
- `scripts/editor_acceptance.py`：真实点击 + 键盘注入 + 宿主侧 FAT32 逐字节校验。

### Fixed
- GUI 的 `q`/`r` 全局快捷键会在文本窗口抢走正文输入（"editor" 丢 r、q 直接退出桌面）：
  现在只在没有文本输入窗口获得焦点时生效；Ctrl+S(0x13) 也转发给 `on_key`。

### Verification
- 点菜单 → 窗口创建 → 输入 `hello editor 42` + 回车 → `[edit] saved 17 bytes` →
  宿主 `EDIT.TXT` = `hello editor 42\n` 逐字节相同 → **5/5 PASS**。
- 回归：内核 13/14、FAT32 宿主 21/21、图片查看器 + GUI 中文 4/4、外观门禁 0 像素。

### Limits
- 回车即保存（解码器不报 CTRL）；固定文件路径；无打开/另存为、无方向键编辑、无撤销/粘贴、
  1 KiB 缓冲、无 CJK 输入。

## [v0.24.0-alpha]

**FAT32 驱动支持多卷：`/mnt/fat`（IDE）与 `/mnt/usb`（U 盘）同一次启动里同时可读可写，
移除 v0.23.0 里如实标注的"fatfs 单卷"限制；宿主解析两个镜像证明写入落在正确卷。
启动呈现与 GUI 外观不变。**

### Changed
- `kernel/fs/fatfs.c`：单卷 `g_vol` → `g_vols[4]` + 当前卷指针；新增
  `fatfs_select_volume/fatfs_current_volume/fatfs_volume_count`；`fatfs_mount()` 返回卷槽号；
  `fatfs_unmount(drive)` 按驱动器清槽并关闭该卷句柄；文件/目录句柄记录所属卷。
- `kernel/fs/fatfs_vfs.c`：挂载点表 `g_mnts[4]` + 前缀匹配 `vol_path()`，每操作先选卷。
- `kernel/main/kmain.c`：USB 挂载段的卷限制文案更新。

### Added
- `scripts/multivol_acceptance.py`：双卷同挂 + 交叉写 + 宿主侧两镜像逐字节校验。

### Verification
- 双卷同挂并各自 `ls`/`cat` 正确；`/mnt/fat` 挂着时写 `/mnt/usb/mv.txt` 成功 → 宿主
  U 盘镜像内容逐字节一致、IDE 卷内容不变 → **12/12 PASS**。
- 单卷回归：FAT32 宿主 21/21、USB 宿主 5/5、内核 13/14、终端中文 10/10、wget 8/8、
  GUI 外观门禁 0 像素。

### Limits
- 卷槽上限 4；挂载点不可嵌套；句柄表仍全局 8+8；ext2/squashfs 仍单卷；无新增并发锁。

## [v0.23.0-alpha]

**第一次把 U 盘当盘用：EHCI（USB 2.0）主控 + 高速枚举 + BOT/SCSI 读写 → 块设备 drive 16 →
FAT32 自动挂 `/mnt/usb`，可 `ls`/`cat`/`write`；宿主侧解析 U 盘镜像逐扇区逐字节验证。
启动呈现与 GUI 外观未变。**

### Added
- `kernel/drivers/usb_ehci.c`（1430 行）+ `usb_ehci.h`：EHCI 端口复位/枚举、异步 QH/qTD、
  BOT、SCSI（INQUIRY/READ CAPACITY/READ(10)/WRITE(10)/TUR/SYNC CACHE）、`blk_register` drive 16、
  开机自检。骨架 HanOS（MIT），BOT 流程参照 HBOS（GPL-3.0），改动见文件头。
- `block.h`：`BLK_TYPE_USB=7`、`BLK_DRIVE_USB=16`；`kmain.c`：`usb_probe()` + `/mnt/usb` 挂载/自检。
- `scripts/mk_usb_testimg.py`、`scripts/verify_usb_disk.py`（宿主侧独立校验）。
- `scripts/mkfont_cjk.py`：字体路径加绝对候选，修掉 git worktree 下构建失败。

### Fixed
- PORTSC 偏移漏加 CAPLENGTH → 读到全 0（看起来像没插设备）。
- CBW/CSW 与数据阶段共用 bounce buffer → CSW 覆盖数据（INQUIRY vendor 空、READ CAPACITY 异常）。
- 高速判据不读 QEMU 恒为 0 的 `PORTSC.PSPD`，改用"复位释放后 PED=1"。

### Verification
- 有 USB：枚举→`storage: 71680 blocks x 512`→`mount /mnt/usb OK`→`selftest PASS`；`ls` 4 条目；
  `write`+`cat`+`umount` 后宿主 `verify_usb_disk.py --allow-write` **8/8**（6 扇区变化全在合法区域、
  W.TXT 逐字节正确、原文件未变）。
- 只读启动整盘 CRC32 `93F4BB22` 未变 → 5/5。
- 无 USB：只多 `[usb] no controller`；13/14 自检、FAT32 21/21、wget 8/8、终端中文 10/10、
  GUI 中文 4/4、外观门禁 0 像素。

### Limits
- 仅高速设备；第一台 mass storage、LUN 0；仅开机枚举无热插拔；无 EHCI HID；fatfs 单卷
  （同时挂 IDE FAT32 时 `/mnt/usb` skipped 并如实失败）；USB CRC 错误路径未验。

## [v0.22.0-alpha]

**第一次能把网上的文件下载进本地盘：`wget http://<ipv4>[:port]/<path> [outfile]`（HTTP/1.0 GET
over 内核 TCP 客户端，边收边写 VFS，Content-Length 校验）。宿主起真实 HTTP 服务器、事后解析
FAT32 镜像逐字节比对相同。启动呈现与 GUI 外观未变。**

### Added
- `kernel/shell/shell.c`：`shell_cmd_wget()` —— URL 解析、HTTP 请求/响应头处理、`Content-Length`
  校验、流式写盘；只接受 IPv4 字面量。
- `scripts/wget_acceptance.py`：宿主 ThreadingHTTPServer + QMP 注入 + 宿主侧 FAT32 解析比对。

### Verification
- guest：`HTTP 200, content-length=3072` → `wrote 3072 bytes`；文本文件 `cat` 内容一致。
- 宿主：`DL.BIN` sha256 与服务端文件完全相同；`HELLO2.TXT` 逐字节相同 → **8/8 PASS**。
- 回归：内核自检 13/14、FAT32 三自检 PASS、终端中文 10/10、GUI 中文 4/4、外观门禁 0 像素。

### Limits
- 仅 IPv4 字面量；HTTP/1.0 GET only（无 HTTPS/POST/重定向/chunked/续传/代理）；响应头 2 KiB
  上限；超时 5 秒；无进度显示。USB 存储在独立分支开发中。

## [v0.21.0-alpha]

**中文从终端走进窗口：GUI 文本绘制支持 UTF-8 与 16×16 点阵（复用 GB2312 子集），图片查看器
底部显示 `缩放 xN`，宿主截图逐像素断言。启动呈现与默认桌面外观未变。**

### Added
- `kernel/gui/gui.c`：`gui_draw_text()` 增加 UTF-8 解码与宽字符 16×16 点阵绘制（两格宽，走
  既有 `gui_px()` 裁剪）；字体子集复用 `kernel/term/cjk16.c`。
- 图片查看器页脚 `scale xN` → `缩放 xN`（新建窗口，不影响默认桌面）。
- `scripts/gui_cjk_acceptance.py`：QMP 点开始菜单 → Image Viewer → 截图 → 按页脚颜色与
  16×16 点阵精确匹配“缩/放”。

### Verification
- GUI：checks=4 ok=4（缩(161,402)、放(177,402) 逐像素相同）。
- 终端回归：10/10（`ls` 中文名 + 滚动后仍精确）。
- 回归：内核自检 13/14、FAT32 三自检 PASS、无 PANIC；外观门禁 0 像素。

### Limits
- 仅新建窗口内中文；窗口标题/开始菜单/默认应用仍英文（冻结外观）。
- GB2312 子集、位图 16×16 不缩放、无输入法、GUI 文本不折行。

## [v0.20.0-alpha]

**终端第一次能显示中文：GB2312 全集 16×16 点阵（7443 字形，SIL OFL-1.1 字体，构建期渲染），
UTF-8 流式解码 + 双宽排布 + 滚动安全的覆盖层；宿主在截图上按 16×16 逐像素断言。
启动呈现与 GUI 外观未变。**

### Added
- `kernel/term/cjk16.c/.h`：16×16 点阵 blob 解析 + 二分查找 + framebuffer 贴图。
- `scripts/mkfont_cjk.py`：构建期从 OFL 字体渲染 GB2312 全集 → `build/font_cjk.bin`（构建自动执行）。
- `kernel/term/term.c`：UTF-8 流式解码、宽字符双空格排布、512 条覆盖层（滚动上移重绘）、
  flush-before-blit 顺序；`flanterm_backends/fb.c/.h` 只读 metrics 访问器。
- `scripts/cjk_terminal_acceptance.py`：启动 + `ls` + 截图 + numpy 全屏字形匹配 + 滚动复验。
- `docs/licenses/OFL-1.1.txt` + provenance/licensing-audit 字体条目。

### Fixed
- 中文首版渲染成实心块/滚动后丢失：点阵写在 framebuffer 后仍 flush 双缓冲栅格，空格被重画；
  → 先 flush 再贴像素，滚动时覆盖层整体上移并重绘。
- 多字节 UTF-8 序列在 term_putc/term_write 之间被拆散：→ term.c 统一流式解码。

### Verification
- 宿主像素断言：`中文长名` 四字形与点阵逐像素相同（中(3,368) 文(21,368) 长(48,368) 名(66,368)）；
  30 回车滚动后仍逐像素相同、整体上移整 7 行 → **10/10 PASS**。
- 回归：内核自检 13/14、FAT32 三自检 PASS、无 PANIC、GUI 外观门禁 0 像素。

### Limits
- 仅 GB2312（7443 字形）；位图固定 16×16；GUI 尚无中文；覆盖层 512 字形、只跟踪滚动；
  中文输入不支持。

## [v0.19.0-alpha]

**终端第一次能自己写文件（`write` / `append` / `hexdump`），只用公开 VFS API，ramfs/FAT32/ext2
一致生效；内容由宿主侧独立解析镜像逐字节核对。启动呈现与 GUI 外观未变。**

### Added
- `write <file> <text...>`：创建/整体替换文本文件，自动补行尾换行，回显字节数。
- `append <file> <text...>`：追加一行（读出 → 补换行 → 整文件写回），回显新大小。
- `hexdump <file> [offset] [length]`：偏移 + 16 字节 hex + ASCII 视图，支持十进制/0x 参数。
- `scripts/shell_text_tools_acceptance.py`：QMP 注入三条命令 + 8 条断言 + 宿主 FAT32 解析。

### Verification
- guest：`write ... (23 bytes)`、`append ... now 36 bytes`、`cat` 两行、`hexdump` 偏移+hex+ASCII。
- 宿主：`NOTE.TXT` 内容与 `"hello shell text tools\nsecond line\n"` 逐字节相同，原夹具文件仍在。
- 回归：内核自检 13/14、FAT32 三自检 PASS、无 PANIC、GUI 外观门禁 0 像素。

### Limits
- write/append 是整文件重写（VFS 无 append 标志），append 上限 2 KiB、write 单行 512 字节。
- CJK 显示仍未进版：本轮做了字体子集 + flanterm 宽字符钩子但渲染成实心块，已整体回退（不发半成品）。

## [v0.18.0-alpha]

**桌面多了图片查看器（BMP/QOI 解码，SutureOS 自绘界面、按需创建所以默认截图不变）；
ring-3 用户态第一次有可用的文件工具（8 个程序 + fork/execve/wait4 的子进程模型）；
修掉一个真 FAT32 bug：目录长到第二个簇以后，新建条目在查找/列目录里“看不见”。
启动呈现与 GUI 外观未变。**

### Added
- **图片查看器**（`kernel/gui/apps.c` + 新增 `img_bmp.c`/`img_qoi.c`/`imgdecode.h`）：
  BMP 1/4/8/16/24/32bpp（BIFF_RGB、top-down/底行优先）与 QOI 全 opcode，无 malloc、有界、
  600×480 上限；状态行显示 `文件 宽x高 格式 (位深)`，整数倍最近邻缩放居中；窗口内点击切换图源。
  解码算法参考 HBOS `src/bmp.c`（GPL-3.0）/ PlantOS `src/plic/qoi.c`（MIT），重写而非拷贝。
- **懒启动**（`gui.h`/`gui.c`/`apps.c`）：开始菜单第一次点图片查看器才 `gui_create_window()`，
  默认桌面仍是 3 个窗口，冻结基线截图逐像素不变。
- **ring-3 文件工具 8 个**：`rls/rcat/rcp/rmv/rrm/rmkdir/rtouch/rdf`（`user/apps/r3tool.h` 薄封装，
  直调 Linux 号系统调用、返回真 -errno）；`ushell` 命令表扩展。
- **内核 ring-3 基础设施三处补齐**：目录 fd + `getdents64`（`FD_FLAG_DIR`、read/write 返 EISDIR、
  fork 不继承游标）、`execve` 的 argv 启动帧、ring-3 fd 从 3 起分配。
- 脚本：`scripts/imgview_acceptance.py`、`scripts/ring3_tools_acceptance.py`；`mkfat32_testimg.py`
  新增 `IMG.BMP`/`IMG.QOI` 夹具。

### Fixed
- **FAT32 多簇目录的目录结尾判定**：`dir_find`/`dir_find_lfn`/`readdir`/`rmdir`/unlink 的 LFN 扫描
  都把第一个 `0x00` 槽当目录结尾；目录长大到第二个簇后，老簇尾部全是 `0x00`，新条目永远扫不到
  （表现为 rename 报成功但新名字打不开、unlink 失败、留下孤儿簇）。新增 `dir_cluster_is_last()`
  查 FAT 链，只有链的最后一个簇的 `0x00` 才算结尾。
- `scripts/verify_fat32_write.py` 同步：读整条根目录链、接受合法的多簇根目录、孤儿簇统计
  把整条根链算作 live。

### Verification
- 图片查看器：真实鼠标（QMP）点开始菜单 → Image Viewer → `[img] decoded /mnt/fat/IMG.BMP 32x32 bmp bpp=24`，
  截图四象限像素精确匹配；点击切换 QOI → `[img] switched to /mnt/fat/IMG.QOI (32x32)` 同样匹配。
- ring-3 工具：`ushell` 22 条命令自断言 **37/37**；宿主 `verify_fat32_write.py` **21/21**；
  无 PANIC、无 user fault；`run probe/hello/forktest` 仍 PASS。
- FAT32：lfn-write 自检 PASS（含 rename/unlink），宿主 21/21，根目录链合法（fat[2]=0x1b）、0 孤儿簇。
- 回归：内核自检 13/14、FAT32 三个自检 PASS、GUI 默认桌面 3 窗口、外观门禁动态矩形外 **0 像素**。

### Limits
- 图片查看器无文件浏览器（冻结 chrome 无列表控件），固定候选路径；最大 600×480；最近邻缩放。
- ring-3：`run` 不能传参；`-r` 递归删除如实拒绝；跨目录 rename 被 VFS 拒绝；touch 不改 mtime；
  `rdf` 是 SutureOS 方言 statfs（两个 u64）。
- FAT32 多簇目录空出的整簇不回收（留作目录空间）；终端仍无 CJK 字体。

## [v0.17.0-alpha]

**系统卷能写了：ext2 从只读驱动变成真正可写（建文件/建目录/删/改名/大文件写）；
根内存盘从 64 节点 / 单文件 2 KiB 扩到 512 节点 / 单文件 256 KiB（堆上按 4 KiB 块增长）。
启动呈现与 GUI 外观未变。**

### Added
- **ext2 可写**（`kernel/fs/ext2.c` 1078 → 2958 行、`ext2.h` 415、`ext2_vfs.c` 516）：
  位图块/inode 分配（每次写后按位图重算组描述符与超级块空闲计数并落盘）、直接+单+双间接写、
  追加/覆盖/truncate（含空洞）、create/mkdir/rmdir/unlink/rename（同目录+跨目录、POSIX 覆盖、
  目录 `..` 改写、祖先环检查、nlink/used_dirs）、VFS 默认 rw 挂载 + `df` 实测计数。
  算法参考 XJ380 / Deshab-OS 的 ext4 实现（Apache-2.0，只读参考，未拷代码）。
- **ext2 写自检 / VFS 写自检 / 位图一致性自检**。
- **ramfs 扩容**（`kernel/vfs/vfs.h`、`vfs.c`）：节点 64→512、单目录子项 64→128；文件数据改为
  堆上 4 KiB 块增长（256 KiB/文件上限，rm 释放）；`df` 按实际分配统计；开机自检写 64 KiB
  并逐字节读回 + 创建 100 个文件。
- **`scripts/verify_ext2_write.py`**（711 行）：独立 ext2 解析器（位图/sb/GDT 计数、inode
  直接+单+双间接块逐字节、目录链、父 nlink、孤儿/重复块、原始文件未改动）。

### Verification
- 1 KiB ext2：写自检 PASS、位图一致、VFS 写自检 PASS；宿主 **44/44**（276 块双间接产物）。
- 4 KiB ext2：同样 PASS → 宿主 **44/44**；同镜像二次启动再验 **44/44**。
- 只读回归：干净镜像 **47/47**、4 KiB **35/35**、三间接夹具 **16/16**。
- ramfs：`[ramfs] capacity self-test: big=65536 bytes chunks=16 files=100 nodes=104/512`，
  `[selftest][xj_vfs] PASS`；shell `df` 131072 1K-blocks、mkdir/touch/ls 正常。
- 回归：FAT32 长名写 + 宿主 21/21、网卡验收两卡 PASS、自检 13/14、无 PANIC；
  GUI 外观门禁动态矩形外 **0 像素**。

### Limits
- ext2 三间接块写入/释放如实拒绝（ENOTSUP）；只写主超级块/GDT（备份副本不更新）；
  4 KiB 测试卷覆盖不到双间接；每次写全量重算位图并 flush，挂 ext2 盘时开机变慢（≥30 s）；
  时间戳不更新、无 symlink/硬链接/xattr、文件上限 4 GiB。
- ramfs 仍是内存盘（256 KiB/文件、512 节点、重启丢数据）。

## [v0.16.0-alpha]

**按 docs/porting-roadmap.md 开始批量移植：光驱数据盘能读（ATAPI + ISO9660）、系统第一次能出声
（AC'97）、FAT32 长文件名能创建/改名/删除（写侧）。启动呈现与 GUI 外观未变。**

### Added
- **ATAPI 光驱 + ISO9660 只读文件系统**：`kernel/drivers/atapi.c/.h`（PIO 轮询：
  IDENTIFY PACKET/PACKET+CDB/READ(10)/READ CAPACITY/REQUEST SENSE，注册只读 blk_device
  驱动号 12..15、2048B 块）+ `kernel/fs/iso9660.c/.h` + `kernel/fs/iso9660_vfs.c`。
  开机扫整段卷描述符集、用 El Torito 引导记录区分启动盘，自动挂 `/mnt/cd`。
  来源：Uinxed-Kernel（Apache-2.0）+ CoolPotOS（MIT）。
- **AC'97 声卡 PCM 输出**：`kernel/drivers/ac97.c/.h`，PCI 探测 + NAM codec + NABM PCM OUT
  32 条 BDL 轮询播放 16-bit 48 kHz；shell 新增 `beep`。来源：HBOS（GPL-3.0）。
- **FAT32 VFAT 长名写侧**（`kernel/fs/fatfs.c`）：自写 LFN 链（N|0x40…1 物理顺序、
  13 个 UTF-16 单元/条、短名校验和）、唯一 8.3 别名、删除/改名清链；UTF-16→UTF-8 读回，
  非 ASCII 长名可按原串打开。规则来源：PlantOS 的 ChaN FatFs 血统（MIT）。
- 验收脚本：`scripts/mkiso9660_testcd.py`、`scripts/verify_iso9660_read.py`（224 项）、
  `scripts/verify_ac97_wav.py`；`scripts/gui_appearance_check.py` 新增 `--live-shot`
  （用同一构建的第二次截图学习"活数据掩码"）。
- `docs/porting-roadmap.md`：素材库 50 仓库的可移植项分 B1..B10 批次。

### Fixed
- `fatfs_open(create)` 永远走不到长名创建分支（`path_resolve` 对不存在的长名直接 ENOENT）
  → 新增"父目录 + 长名组件"解析路径。
- unlink/rmdir 只删短名条目、长名链变孤儿 → 删除/改名时先标记整条 LFN 链。
- 长名写测试后 FSInfo 与实测差 1 簇 → 自检末尾补 `vfs_sync()`。
- 外观门禁误报：Task Manager 活数据数字超出旧动态矩形（pristine v0.15.0 也报 29 px）
  → 门禁升级为"动态矩形 + 双次运行活数据掩码"。

### Verification
- ISO9660：`mount drive=15 vol=SUTURE_CD files=4`、`selftest PASS`；三级目录 / 跨 3 块
  4680 B 文件 / 60 条目录项全部命中；宿主 `verify_iso9660_read.py` **224/224**。
- AC'97：WAV 实录 **48000 Hz / 1.000 s / 0.5000 FS / 440.01 Hz** 全 PASS。
- FAT32：`lfn-write self-test PASS (create=1 readdir=1 rename=1 unlink=1 utf8=1)`；
  宿主 `verify_fat32_write.py` **21/21**。
- 回归：自检 13/14、`syscalls: 81`、`nic_acceptance.py` 两网卡 PASS、无 PANIC；
  GUI 外观门禁矩形外 **0 像素**。

### Limits
- ISO9660 只读、仅主卷（Joliet/Rock Ridge/multi-extent/UDF 未做）、大写 8.3 名、单卷单挂；
  ATAPI 仅 PIO、单条 READ(10) ≤8 块、无换盘检测。
- AC'97 单次 ≤2.0 s、原生 48 kHz、纯轮询、不录音；MMIO BAR 分支未真机验证。
- FAT32 长名写仍限同目录 rename；非 ASCII 名终端显示乱码（无 CJK 字体，下一批做）。
- About 窗口 "14 upstream hobby OS projects" 属冻结外观文案，未改；真实上游数（16）见 README。

## [v0.15.0-alpha]

**两块从素材库移植的真实能力：Intel e1000 网卡（VMware 下第一次有网络）+ FAT32 VFAT 长文件名
读取；启动方式与 GUI 外观未变（有门禁脚本证明）。**

> 版本号从 v0.11.5 跳到 v0.15.0：`v0.12.0/0.13.0/0.14.0-alpha` 三个 tag 指向上一轮被判
> "外观被改动"且从未合入 main 的 `wip/all-ports` 提交；按发布纪律旧 tag 不移动不删除，
> 因此新版本使用未占用的 0.15.0，与旧 tag 共存。

### Added
- **Intel e1000 (8254x) 网卡驱动**（`kernel/drivers/e1000.c` 354 行 + `e1000.h`）：
  移植自 **Cinux** `kernel/driver/net/e1000.cpp`（**MIT**）。VMware Workstation 只提供
  e1000 虚拟网卡，此前 VMware 下完全没有网络。
- **链路层分发**（`kernel/net/net.c`）：新增 `ll_send()` / `ll_poll_rx()`，IP 栈不再直连
  RTL8139；选择顺序为 **RTL8139 优先、e1000 回退**（VMware 只有 e1000）。
- **FAT32 VFAT 长文件名读侧**（`kernel/fs/fatfs.c` +243 行）：`readdir` 拼回长名（`ls` 可见），
  新增 `resolve_component()` 让 `open/stat/cat` 也能用长名打开（wip 分支只做了显示这一半）；
  校验和不匹配的链丢弃，删除/改名时清理链。
- **长名开机自检**：`[fat32] lfn self-test PASS (open=1 payload=1 readdir=1)`。
- **`scripts/nic_acceptance.py`**：分别用 `-device rtl8139` 与 `-device e1000` 启动，
  注入 `ping 10.0.2.2`，断言链路行与 ≥3 条回复；两块网卡各自 PASS。
- **`scripts/gui_appearance_check.py`**：外观门禁，只容忍文档化的动态数值矩形。
- **`e1000_diag()`**：netstat / 自检超时时输出 GPRC/RDH/RDT/描述符状态。
- **文档**：`docs/audit-unfinished.md`（未完成项审计：P0=7/P1=19/P2=30 + 21 处过期文档条目）、
  `docs/port-candidates.md`（50 个开源自制 OS 的移植候选与许可评估）、
  `docs/licensing-audit.md`（上游许可证实读核对表）。

### Fixed
- **wip 分支不可直接 merge**：它基于 v0.11.0，merge 会删掉 v0.11.1–v0.11.5 的
  fork/socket/signal/GUI 工作；本版在 v0.11.5 上重新实现。
- **e1000 出处许可写错**：头注释原写 "Cinux GPL-3.0"，实际 Cinux/LICENSE 为 **MIT**，已改正。

### Verification
- `py -3 scripts/nic_acceptance.py` → **overall PASS**：rtl8139 开机自检
  `[net] selftest ping 10.0.2.2 -> reply in 1178 us` + 交互 ping 3/3；
  e1000 `[net] e1000 found at 0xfebc0000` + 交互 ping 3/3。
- `py -3 scripts/verify_fat32_write.py build/fat32_ata.img build/fat32.img` → **17/17**。
- FAT32 `ops self-test PASS (3/3)`、模块自检 `13/14`（`moos_intr` 为既有项）、
  `syscalls: 81 registered`、无 PANIC、启动日志 266 行（基线同）。
- GUI 外观：`scripts/gui_appearance_check.py` → 动态矩形外差异 **0 像素**。

### Limits
- **e1000 开机自检 ping 在 QEMU 11.1 下超时**（`GPRC=0`，设备模型不投递），同一次启动里
  交互 ping 3/3 正常；wip 上一轮同样复现，非本版引入。验收以 `scripts/nic_acceptance.py` 为准。
- e1000 为轮询驱动；FAT32 长名只读，写侧仍是 8.3；VMware 真正上网仍需先 `dhcp`。
- **SpiritFoxOS 上游没有 LICENSE 文件**：历史网络/块设备模块出处依据不足，已登记
  `docs/licensing-audit.md` 作为待确认项（本版新增代码不再从其拷代码）。

## [v0.11.5-alpha]

**GUI 窗口可以拖动、缩放、关闭了**（界面外观不变）。

### Fixed
- **拖动跳跃**：`gui_drag_begin_move/resize` 用"排队的点击坐标"算抓取偏移，而
  `gui_handle_mouse()` 先刷新实时指针再排空点击队列，同帧内的位移不在点击记录里
  → 第一帧跳一大截（实测按在 x=221、指针在 x=100，窗口被甩 100px）。改为以**实时指针**
  为基准并钳进屏幕。
- **右键误触发**：`gui_click_at()` 完全忽略 button，PS/2 的 bit1（右键）会开始标题栏拖动、
  在红色 x 上还会**关闭窗口**。现在只认左键，并记录 `g_click_right`。
- **指针回跳**：`gui_pointer_moved()` 把可见指针倒回排队的点击位置，导致拖动手势拿到旧位置。
  已删除该回跳。

### Verification
- 真 QEMU 交互（QMP 注入鼠标 + 截图分析）：拖动 `[gui] drag: 'Task Manager' at 24,24`
  标题栏像素恰好 +120/+80、旧矩形为纯桌面渐变、全桌面 0 残留像素；缩放
  `[gui] resize: 'Task Manager' 330x260` 边框恰好 +100/+60、0 残留；关闭
  `[gui] window closed:` 后矩形变桌面渐变，开始菜单可重新打开。
- 外观：与 `docs/baseline/base_gui.png` 对比，317 px 差异在动态文字区内，另有 78 px
  在 2 个簇内且都证实是动态文本（Task Manager 的 PMM 数字、About 的版本号数字——
  基线是 v0.11.0 截图，当前是 v0.11.4）；默认画面新增像素 = 0。
- 回归：启动日志 265 行、`syscalls: 81 registered`、自检 13/14、FAT32 ops PASS、
  无 PANIC。

## [v0.11.4-alpha]

**真正的多进程**：fork（写时复制）+ wait4 回收 + getppid，ring-3 shell 也能 fork/wait。
启动呈现与 GUI 外观零变化。

### Added
- **进程表与协作式进程调度**（`kernel/syscall/process.c/.h`）：每个进程 =
  {userspace_t + 16 KiB PMM 内核栈 + 预构建 user_context_t}；切换是
  push rbx/rbp/r12-r15 -> swap rsp -> pop -> ret，再切 CR3 与 `%gs:8`。
  子进程以 RUNNABLE 状态挂在一个手工构造的帧上，`ret` 落到 `proc_child_start` -> iretq
  且 `rax=0`。
- **fork + COW**（`userspace.c`）：PTE 软件位 9 = `PTE_COW`，父子共享同一批物理页并
  置只读；缺页时按 160 项"物理帧 -> 引用计数"表决定"复制"还是"仅恢复写权限"。
  CPL=3 与 CPL=0 两种缺页都先走 COW 再考虑信号。
- **wait4（61）/ getppid（110）/ getpid** 真实现：子进程退出成僵尸，父进程 wait4 拿
  `status = sig<<8 | code` 并回收槽位、地址空间与 fd；无子进程返回 `-ECHILD`，
  `WNOHANG` 支持。fd 表与 signal 槽位随 fork 克隆。
- **forktest**（`user/apps/forktest.c`）ring-3 验收程序；ring-3 shell 新增 `fork` 命令。
  另注册 clone(56)/vfork(58)/setpgid(109)/getpgrp(111)/getsid(124) -> **81 个系统调用**。

### Fixed
- **子进程经 int 0x80 退出会把 GS MSR 对留在"用户侧"**（该门不做 swapgs），父进程恢复后
  的 swapgs 反转了这对 MSR，下一次 syscall 从线性地址 8 取内核栈（RSP=0xf000ff53f000e2c3）
  导致整机停止打印；现在每次进程切换都把 GS_BASE/KERNEL_GS_BASE 归一化到内核侧。
- 删除 mmu.c 里从静态 MMU 池复制且无引用计数的 COW 辅助函数（双重释放风险）。

### Verification
- 干净重建 build=0、ISO 生成；启动日志 **265 行 == 基线**、`syscalls: 81 registered`、
  自检 13/14、`ring3 9/9`、`userspace 9/9`、FAT32 写+ops PASS、无 PANIC。
- `run forktest`：父 pid=1/子 pid=2、子写 'C' 后父仍读到 'Q'（COW 成立）、
  `wait4 -> pid=2 status=0x2a (code=42)`、无子进程时 `-ECHILD`、第二个子进程 status 0x7，
  `RESULT PASS`，退出 0；ring-3 shell `fork` 同样 PASS；`run sigtest` 回归 PASS。

### Limits
- 协作式调度（fork 返回 / wait4 阻塞 / yield / exit 才是切换点），无抢占；自旋子进程会
  饿死兄弟。
- 一次只有一个会话，root 退出会销毁存活子进程；fork 只复制 16 页用户窗口（mmap/brk 区
  不继承）；clone(CLONE_VM)/线程返回 `-EINVAL`，vfork == fork。
- wait4 status 编码是 `sig<<8|code`（不是 Linux 的 `(code&0xff)<<8`）。

## [v0.11.3-alpha]

补完第 4、5 项：socket 系统调用 + DHCP/DNS 客户端 + 真正的 signal 语义（sigframe +
rt_sigreturn）。启动呈现与 GUI 外观零变化。

### Added
- **socket 系统调用家族**（`kernel/syscall/sockets.c/.h`）：socket/connect/accept/sendto/
  recvfrom/sendmsg/recvmsg/shutdown/bind/listen/getsockname/getpeername/setsockopt/
  getsockopt 注册到内核既有 UDP/TCP 对象上，带用户指针校验；socket fd 进入 fd 表。
  `syscalls: 76 registered`（v0.11.2 为 62）。
- **DHCP 客户端**（`kernel/net/dhcp.c/.h`）：DISCOVER/REQUEST over UDP 68->67，解析
  OFFER/ACK 后写入 IP/掩码/网关/DNS。**只在 shell 命令 `dhcp` 时运行，不在开机阶段发起。**
- **DNS 解析器**（`kernel/net/dns.c/.h`）：A 查询构造 + 应答解析，`nslookup <name>` 触发。
- **signal 真语义**（`kernel/syscall/signal.c`）：进入 handler 前在用户栈上构造 sigframe
  （保存完整上下文 + 返回跳板），`rt_sigreturn` 从 sigframe 恢复全部寄存器与
  rip/rsp/rflags，handler 返回后精确回到被打断处；`sigaction`/`sigprocmask` 掩码真实
  生效；`SIG_DFL` 仍是终止该进程（不停机）。
- **ring-3 验收程序 `user/apps/sigtest.c`**：安装 SIGUSR1 handler -> `kill(getpid())`
  -> 校验 handler 运行并通过跳板返回 -> 成功退出 0；注册进内建程序表（`run sigtest`）。

### Verification
- 启动日志：`syscalls: 76 registered`、自检 13/14、`ring3 selftest 9/9`、
  `userspace selftest 9/9`、`[fat32] write self-test PASS`、无 PANIC；
  **启动日志行数 265 == 基线 265，零增删行**（只有数据值不同）。
- 新自检全部"成功不打印、失败才打印"。
- GUI 目录零改动，启动 print 序列未改动。

## [v0.11.2-alpha]

补完 5 项未完成系统调用中的前 3 项（dup/*at/ioctl+tty）。**启动呈现与 GUI 外观零变化。**

### Added
- **`dup`/`dup2`/`dup3`（32/33/292）**：fd 表新增"复制条目"能力；`dup` 取最小空闲 fd，
  `dup2` 指定目标 fd（已打开先关闭），管道端点复制会正确增加读写引用计数以保持
  EOF 语义；不再返回 `-ENOSYS`。
- **`creat`（85）**：等价 `open(path, O_CREAT|O_WRONLY|O_TRUNC)`。
- **`openat`/`mkdirat`/`unlinkat`/`renameat`/`fstatat`（257/258/263/264/262）**：
  `AT_FDCWD` 走当前 cwd，真实 dirfd 经 fd 表解析后拼接相对路径，再复用非 `*at` 版本
  的 VFS 实现。
- **`statx`（332）**：填充 `struct statx` 基础统计（mask=STATX_BASIC_STATS），
  时间戳按秒/纳秒给出。
- **最小 tty 层（`kernel/vfs/tty.c`/`.h`）**：fd 0/1/2 视为控制台 tty，
  `ioctl` 支持 `TCGETS/TCSETS/TCSETSW/TCSETSF`（termios 存取）、`TIOCGWINSZ`
  （返回控制台行列数）、`TCFLSH`；其它请求仍返回 `-ENOTTY`。原来所有 `ioctl`
  都是 `-ENOTTY`。

### Verification
- 启动日志：`syscalls: 62 registered`（v0.11.1 为 56，新增 6 个）。
- 回归不变：自检 13/14（`moos_intr` 既有判定）、`ring3 selftest 9/9`、
  `userspace selftest 9/9`、无 PANIC。
- 新增自检（**成功不打印**，失败才打印）：dup 双写同一文件、dup2 到固定 fd、
  `openat(AT_FDCWD,..)` 与非 at 版本一致、`statx` 返回 size>0、`ioctl(TCGETS)`/`TIOCGWINSZ` 成功。

### Not in this release
- **socket 家族（41-50）与 DHCP/DNS**：未做（下一批）
- **signal 真语义（sigframe + `rt_sigreturn`）**：未做（下一批）；当前 `SIG_DFL` 只杀进程、
  handler 仍只改 rip

## [v0.11.1-alpha]

补完三项"未完成/坏了"的部分，**启动呈现与 GUI 外观零变化**（见验证）。

### Fixed
- **用户态 ABI 号段冲突（真 bug）**：`user/lib/vimtu64.h` 把 9/10/11 声明为
  `FB_MAP`/`FB_FLIP`/`FB_PRESENT`、12/13/14 为 `INPUT_POLL`/`SHM_CREATE`/`SHM_MAP`，
  而内核 `int80.c:47` 把 **9 用作 execve**——用户一调 `fb_map()` 就会执行 execve。
  内核尚未实现这 6 个功能，现改为保留号段 `0x40..0x45` 并注明"调用返回 -ENOSYS"。
- **`SIG_DFL` 会停机整机**：`signal.c` 的默认动作原本调 `sched_shutdown()`，任何
  `SIG_DFL` 投递都会关掉整台机器。现在只终止该用户程序（退出码 128+sig），
  内核上下文的投递直接丢弃。

### Added
- **`mprotect`（系统调用 10）**：真实的 PTE 权限变更（W/NX 位）+ TLB 刷新；
  未映射页返回 `-ENOMEM`，非页对齐/零长返回 `-EINVAL`。
- **`munmap` 真实解除映射**：原实现是空壳（只 `return 0`）。现在清 PTE 并刷 TLB，
  unmap 后再访问会缺页而不是"静默还能用"（物理页暂不归还 PMM，注释已写明）。

### Verification
- 新增开机自检（成功时**不打印**，失败才打印，保持启动日志与基线一致）：
  `mmap -> mprotect -> munmap` 往返 + ABI 号段检查 = PASS。
- 回归不变：自检 13/14、`syscalls: 56 registered`、`ring3 selftest 9/9`、
  `userspace selftest 9/9`、FAT32 写+目录操作 PASS。
- **外观未变（按新规矩逐项验）**：
  - 终端截图与 `docs/baseline/base_terminal.png` **逐像素完全相同**（`bbox=None`）
  - GUI 截图与基线差异仅在 `(44,102)-(352,221)` = Task Manager 的实时计数器文字
    （运行时长/帧数），窗口、任务栏、配色、字体均一致
  - 启动日志逐行对比：仅内核地址（+0x1000）、PMM 空闲页数（-1）、RTC 时间不同，
    流程与文案完全一致

## [v0.11.0-alpha]

The userspace becomes real: interrupts stay enabled at CPL=3, every program runs in
its own address space (CR3 switch), `execve` replaces a running program, and there is
an interactive ring-3 shell.

### Added
- **Per-process address spaces** (`kernel/syscall/userspace.c`): a real PML4 per
  program - kernel map shared, the 1 GiB..2 GiB slot (free in the kernel map) gets a
  private PDPT/PD/PT for the user window at 0x40000000, so two processes can use the
  same VA and see different pages. CR3 is switched on entry to ring 3 and back on
  exit. Page-table pages come from the PMM (the static MMU pool ran out after a few
  runs), and are released on exit.
- **Ring 3 runs with IF=1**: hardware interrupts are delivered while a program is at
  CPL=3 (the timer keeps ticking), so userspace is preemptible in principle.
- **`execve`** as own-ABI `int 0x80` number 9: rebuilds the current address space
  with another built-in program and rewrites the interrupt frame (rip/rsp/rax) so
  `iretq` lands at the new entry point. Used by the shell's `hello`/`probe` commands.
- **Ring-3 interactive shell** (`user/apps/shell.c`, 6889 B): reads commands through
  `int 0x80` read(fd=0) (the kernel echoes them), and supports help/hello/probe/
  crash/pid/ticks/exec/exit. The kernel shell enters it with `ushell`.
- **Console input for ring 3**: `int 0x80` read on fd 0 is routed to the keyboard
  ring with echo and backspace handling.
- **`userspace_selftest`** (9 checks) wired into the boot log.

### Fixed
- **`el0_enter_user_saved` switched CR3 after zeroing the registers**, which cleared
  `%rdi` (the PML4 argument) - so CR3 was never switched and ring 3 ran on the kernel
  map where 0x40000000 is unmapped (QEMU: CPL=3 #PF err=0x14 at 0x40000000 forever).
- **The window is at 1 GiB, i.e. PML4 index 0** (not 1, which starts at 512 GiB), so
  the private PDPT belongs in `pml4[0]`.
- **#PF from CPL=3** was handled by the COW path, which ended in
  `sig_send(current=-1, SIGSEGV)` and re-executed the faulting instruction forever.
  A user-mode page fault now kills the program (exit 128+14) and the kernel survives:
  `[ring3] user #PF at 0x100000 (err=0x7) -> killed (exit 142)`.
- **GS MSRs are saved/restored across ring 3**: a `syscall`-initiated exit
  (`SYS_EXIT`) long-jumps out of the entry path and skipped the matching `swapgs`,
  leaving GS swapped for the next entry.
- Two shell input bugs: the prompt was reprinted on every `-EAGAIN`, and a single
  keystroke was treated as a command (read(0) returns as soon as one char is
  available, so the shell accumulates until newline).

### Verification
- `userspace selftest: RESULT PASS (9/9)`, `ring3 selftest 9/9`,
  `int80 selftest 2/2`, regression otherwise unchanged (self-tests 13/14, rtc 8/8,
  pipe 16/16, FAT32 write+ops PASS, host 17/17, AP online, ping, mouse, sudo 23/23).
- Interactive ring-3 shell transcript: `help`/`ticks`/`pid` work, `hello` execve's the
  libc program in the same address space, `crash` (write to kernel memory) is killed
  with exit 142 while the kernel survives, and `ushell` can be re-entered repeatedly.

### Known limitations
- No `fork`/`wait4`: one process at a time (the scheduler still has the known
  dual-scheduler problem), so the shell cannot run a program in the background and
  return to itself - `execve` replaces it.
- The userspace helpers are in the kernel image (no ELF loader on the boot path yet).
- Interrupts *during* a syscall stay disabled (the ISR stub frame handling at CPL=3
  is exercised by the timer, not by nested re-entry).

## [v0.10.1-alpha]

The ported userland libc runs. v0.10.0 shipped the port with one diagnosed blocker
(the `int 0x80` frame layout); this patch fixes it and enables the programs.

### Fixed
- **The `int 0x80` frame offsets.** `struct moos_regs` declares `rax` at +0x00, but
  the shared ISR stub pushes the GPRs as `rax, rcx, rdx, rbx, rsi, rdi, rbp,
  r8..r15`, so in memory the layout is `r15 ... rcx, rax` - i.e. rax is at **+0x70**
  (the struct's tail, vector +0x78 / error +0x80 / rip +0x88, is what really
  matches, which is why every existing handler worked). `kernel/syscall/int80.c`
  now reads the arguments and writes the result with explicit offsets
  (`FRAME_RAX/RDI/RSI/RDX`). Before this, the handler saw `nr=0` / `nr=-38` and the
  libc programs hung in ring 3 with no output.

### Verification: the ported libc runs
```
[user] running 'probe': va=0x40000000 len=10249 stack=0x4000fff0
[probe] 1 int80 write ok
[probe] 2 puts ok
[probe] 3 printf ok argc=1 argv0=user64
[probe] 4 malloc ok len=32 str=abcdefghijklmnopqrstuvwxyzabcdef
[probe] 5 pid=0 ticks=1792728129
[probe] 6 exiting with 7
[user] 'probe' exited with 7
[user] running 'hello': va=0x40000000 len=5161 stack=0x4000fff0
hello from Vimtu64 C userland
[HELLO] argc=1 argv0=user64 pid=0 ticks=1792728129
[user] 'hello' exited with 0
int80 selftest: RESULT PASS (2/2)   (14 calls served, 0 -ENOSYS)
```
That is: a C program built against the ported libc runs at CPL=3, its `printf`
reaches the console through `int 0x80` -> `write(2)`, `malloc` and `string.h`
work, and its exit code comes back to the kernel.

### Regression
self-tests 13/14, rtc 8/8, pipe 16/16, ring3test 9/9, FAT32 write+ops PASS, host
17/17, AP 1 online, RTL8139 ping, PS/2 mouse, sudo 23/23 - all unchanged.

## [v0.10.0-alpha]

Ported a real userland libc from VimtuOS and wired the machinery to run it, which
uncovered and fixed three genuine kernel bugs. The ported programs themselves are
**not runnable yet** - the `int 0x80` own-ABI gate reads the wrong call number
(diagnosed below) - so this release ships the infrastructure and the kernel fixes,
not the milestone.

### Added
- **`user/lib/`** - VimtuOS' minimal libc, ported nearly verbatim (GPL-3.0, same
  project family): `string.c`, `stdlib.c` (real `malloc`/`free` free-list arena),
  `stdio.c` (buffered `printf`/`puts`), `syscall.c`/`syscall.S` (own ABI +
  Linux-number fallbacks), `crt0.S` (entry, `main(argc,argv,envp)`, `exit`), and
  its headers (`stdio.h`, `stdlib.h`, `string.h`, `unistd.h`, `errno.h`,
  `unistd.h`, `vimtu64.h`, `sys/types.h`, `sys/stat.h`, ...).
- **`user/lib/user.ld`** - flat-blob linker script at VA `0x40000000` (VimtuOS used
  4 GiB); `.text.start` first, `.l*` sections for `-mcmodel=large`, `.image_end`
  sentinel so the `.bss` tail is part of the blob.
- **`user/apps/hello.c`** and **`user/apps/probe.c`** (staged acceptance program:
  int-0x80 write -> puts -> printf -> malloc -> getpid/ticks -> exit(7)).
- **Build integration**: `scripts/build.py` compiles the libc once, links each app
  with the flat-blob script and embeds the blob in the kernel image
  (`_binary_<name>_bin_start/end/size`), 32 KiB load-window limit enforced.
- **`kernel/syscall/int80.c`** - the Vimtu64 own-ABI gate on vector 0x80 (DPL=3):
  write/read/open/close/getpid/ticks/sleep_ms/exit forwarded onto the Linux-numbered
  syscall table; everything else answers -ENOSYS.
- **`mmu_map_user_pages()` / `mmu_prepare_user_range()`** - map a fixed user VA
  window onto PMM pages (U/S=1, W for data, NX for stack), which is what a program
  linked at an absolute address needs.
- **`user_run_blob()` / `user_run_builtin()` / shell `run`** - the general ring-3
  program launcher (the v0.9.0 runtime only knew the one embedded asm blob).

### Fixed (all three found by this work)
- **`IA32_EFER.NXE` was never set.** `mmu_map_user_pages()` marks the user stack
  non-executable with PTE bit 63; with NXE off that bit is *reserved*, so every
  stack access raised a reserved-bit #PF. Symptom: the machine stopped silently
  right after "zeroed page 7" (QEMU `-d int` showed `v=0e CR2=0x0`).
- **`el0_resume_kernel()` violated the C ABI** (SysV): it restored only `rsp` and
  returned, leaving `rbx`/`rbp`/`r12-r15` holding whatever the user program left
  in them, so the caller's locals were garbage (`console_printf("%s", name)` with
  name == -38 -> #PF inside `console_printf`, CR2=0xffffffffffffffda). It now
  saves those registers in `el0_enter_user_saved()` and pops them on the way back.
- **`sys_exit()` indexed `sched_cpu.tasks[tid]` with `tid == -1`** when the kernel
  itself is the caller (the ring-3 programs run from kmain), writing out of bounds
  and corrupting kernel state. Guarded like the fd syscalls.

### Known blocker (why the ported programs do not run yet)
`int 0x80` *works* - the asm probe program calls `getpid` through it and gets an
answer - but for the libc programs the handler reads the wrong call number
(`nr=0` / `nr=-38` in the log). The reason: `struct moos_regs` declares `rax` at
offset +0x00, while the shared ISR stub pushes `rax` first, so it lands at +0x70.
The proper fix is a dedicated `int 0x80` stub that passes `(nr, a1, a2, a3)` to C
in registers instead of through that frame. Until then `user_run_builtin()`
refuses to start the libc programs (running one hangs the machine) and says so.

### Verification
- Regression unchanged and green: self-tests 13/14, FAT32 write+ops PASS, host
  17/17, AP 1 online, RTL8139 ping, PS/2 mouse, rtc 8/8, pipe 16/16, sudo 23/23,
  `ring3test` 9/9, and the asm ring-3 program still prints
  `hello from ring 3` / `int80 getpid=3` / `uname sysname=Linux` and exits 42.
- `hello` (5161 B) and `probe` (10249 B) build and link at 0x40000000 and are
  embedded in the kernel; the loader maps them, copies them and enters ring 3
  (QEMU `-d int` shows the calls), but the own-ABI frame bug stops them there.

## [v0.9.0-alpha]

**Ring 3 runs.** The audit's biggest gap - "everything is in ring 0, there is no
userland" - is closed: a real CPL=3 program executes, makes syscalls through the
`syscall` instruction, gets kernel data copied into its own buffer, and returns
an exit code to the kernel.

### Added
- **`user/init.S`**: the first user program (334-byte flat image, no libc). It
  calls `write(1, ...)`, `uname()`, `getpid()` and `exit(42)`.
- **`kernel/syscall/ring3.c`**: the ring-3 runtime - PMM-owned user pages mapped
  with U/S=1, SSE enabled (CR4.OSFXSR|OSXMMEXCPT, CR0.EM/TS clear), GS base set up
  for the syscall entry, `el0_validate` armed, 16 KiB dedicated syscall stack.
- **`el0_enter_user_saved()` / `el0_resume_kernel()`**: enter CPL=3 and come back
  when the program exits (`sys_exit` recognises a ring-3 caller).
- **Fault isolation**: an exception taken at CPL=3 kills the user program with
  Linux's `128+signal` convention and the kernel keeps running instead of
  panicking (or, worse, re-executing the fault forever).
- **`ring3` / `ring3test`** shell commands + a boot-time run with a 9-check
  acceptance suite.

### Fixed (each of these blocked ring 3)
- **`el0_enter_user()` was broken three ways** and had never been called: it
  pushed/popped its two arguments in the wrong order (CR3 got the *context
  pointer*), every `user_context_t` offset was wrong (fs_base read from offset
  112 = rax), and it built an iret frame then executed `sysretq`. Rewritten for a
  single address space with `iretq`.
- **`syscall.S` built an incomplete frame** (only the 15 GPRs), so `ctx->cs` /
  `ctx->rip` were read from stale stack - which is why the signal-delivery hook
  never fired. It now saves a complete `user_context_t` and returns with `iretq`.
- **`IA32_EFER.SCE` was never set**, so the very first `syscall` raised #UD
  (vector 6 at user rip+0x1c). Also fixed `STAR`: the SYSCALL field held 0x10
  (kernel *data*) instead of 0x08 (kernel *code*), and the SYSRET field was wrong.
- **`fd_tables_init()` was never called**: the per-task fd table was all zeros,
  so `write(1)` did not take the stdout path and `open()` from ring 3 would always
  have failed with EMFILE.
- **`sched_cpu.current == -1`** while the kernel itself makes syscalls (the
  boot-time ring-3 run) indexed `fd_tables[-1]` out of bounds and swallowed the
  program's output. fd 1/2 are now resolved before the table lookup, and every
  fd syscall validates the task slot.
- **The ring-3 syscall ran on the launcher's stack**, overwriting the return
  address that `el0_enter_user_saved()` had saved - so `exit(60)` "returned" into
  garbage (observed: kernel #UD at rip=0x3). It has a dedicated stack now.

### Verification
- Boot log with the program running end to end:
  ```
  [ring3] entering CPL=3: entry=0x0000000000daa000 ...
  hello from ring 3
  uname sysname=Linux
  pid=0
  [ring3] the user program returned (exit=42)
  [  OK  ] ring3: first user program ran, exit=42
  ```
- `ring3test`: **9/9 PASS** (blob, user pages, SSE, FPU flags, GS base, program
  ran, exit code 42, user PTE U=1, kernel text U=0).
- Regression unchanged: self-tests 13/14, FAT32 write+ops PASS, host 17/17,
  AP 1 online, RTL8139 ping, PS/2 mouse, rtc 8/8, pipe 16/16, sudo 23/23.

### Known limitations
- The program runs with **IF=0**: a hardware interrupt taken at CPL=3 does not
  return correctly yet (the legacy same-CPL ISR stubs assume the kernel frame),
  so interrupts are re-enabled only after control is back in ring 0. The program
  needs no timer; a real ring-3 preemptive path is the next step.
- One shared page table with U/S bits rather than a per-process address space, so
  no `fork`/`execve` of a second image yet; the ELF loader (`loader.c`) is wired
  to the new entry but unused by the boot path.
- Still no libc in ring 3 (the program is hand-written assembly), and
  `dup`/`ftruncate`/`readlink`/`symlink` remain `-ENOSYS` as documented in
  v0.8.0-alpha.

## [v0.8.0-alpha]

The ring-0 half of the audit's "unfinished" list: the syscall surface a libc
needs, a real wall clock, pipes, file-management commands, and signal delivery
wired into the one return path this kernel has.

### Added
- **56 syscalls registered** (was 22). New: `stat`/`fstat`/`lstat`, `access`,
  `lseek`, `ioctl`, `dup`/`dup2`, `fsync`/`fdatasync`/`sync`, `truncate`,
  `statfs`, `chdir`/`getcwd`, `mkdir`/`rmdir`/`unlink`/`rename`, `getdents64`,
  `chmod`/`umask`, `getpid`/`gettid`/`getppid`, `getuid`/`geteuid`/`getgid`/
  `getegid`/`setuid`/`setgid`, `uname`, `gettimeofday`/`time`/`clock_gettime`
  (now the real clock)/`nanosleep`/`times`, `kill`/`wait4`/`exit_group`,
  `sysinfo`/`getrlimit`/`setrlimit`/`prctl`/`sched_getaffinity`/`getcpu`,
  `pipe`/`pipe2`. Relative paths resolve against the cwd, `stat` fills Linux's
  byte-exact 144-byte `struct stat`, and `uname` reports `sysname = "Linux"` with
  release `6.1.0-sutureos` so a libc's checks pass.
- **CMOS RTC driver** (`arch/x86_64/rtc.c`): the wall clock was faked (a constant
  1234567890); it now reads the RTC once at boot (BCD/binary, 12h/24h, century
  register) and anchors the tick counter on it. `date` prints real UTC.
- **Pipes** (`kernel/ipc/pipe.c`): pipe fds live in their own number space and
  `read`/`write`/`close` route to them; EOF when writers close, `-EPIPE` with no
  readers, short writes when full, bounded wait when empty.
- **Shell**: `rm`, `mkdir`, `rmdir`, `touch`, `cp`, `mv`, `date`, `uname`, `free`,
  and three acceptance commands `systest` / `pipetest` / `rtctest`.
- **Signal delivery**: `sig_check_pending()` is called on the syscall return path
  for user tasks (it had no call site at all), and `kill(2)` delivers into it.

### Fixed
- **ramfs `rename` was a stub** returning `VFS_EPERM`: implemented as a
  same-directory rename (what `mv a b` in one directory needs) with EEXIST/EXDEV
  checks, which is what made `systest`'s rename check fail at first.
- **RTC 12-hour decoding**: the PM flag (bit 7) was still present when the hour
  was converted, so "1 PM" became 141 instead of 13. Strip it before converting.
- RTC init decoded the year twice (BCD-decoding an already-binary year), which
  made every read look implausible and left `date` at 1970-01-01.
- `date` padding: this kernel's `snprintf` has no `0` flag, so the formatter now
  pads manually (`2026-10-22 23:04:11 UTC`, not `2026-10- 1  1: 4:11`).
- The self tests used `%-43s`, which `console_printf` prints literally (its
  formatter has no `-` flag) - replaced with plain `%s`.

### Verification
- `systest`: **39/39 PASS** (stat/lseek/read/write/fstat, mkdir/getdents64/
  unlink/rmdir, rename, chdir/getcwd, getpid/getuid/geteuid, uname, real
  clock_gettime/gettimeofday/time, pipe over the syscall path, sysinfo,
  getrlimit, sync, and the honest -ENOSYS/-ENOTTY answers).
- `pipetest`: **16/16 PASS**; `rtctest`: **8/8 PASS** (BCD/binary/12h/leap/format).
- Interactive: `date` -> `2026-10-22 23:06:17 UTC`; `mkdir /demo`, `touch`,
  `cp /etc/motd /demo/b.txt` (92 bytes), `ls` (a.txt, b.txt), `mv` (c.txt),
  `rm`, `rmdir`, `free`, `uname -a` all worked.
- Regression unchanged: self-tests 13/14, FAT32 write+ops PASS, host 17/17,
  AP 1 online, RTL8139 ping, PS/2 mouse, sudo 23/23.

### Still open (the ring-3 half, next batch)
- No userland yet: `/init` is never executed, `dynlink.c` is unreferenced, and
  `dup`/`dup2`/`ftruncate`/`readlink`/`symlink` answer `-ENOSYS` because the fd
  table has no dup API and the VFS has no symlinks. e1000 (VMware networking),
  FAT32 LFN, ext2 write and the GUI work are untouched.

## [v0.7.0-alpha]

First real privilege model: uids/gids, `/etc/passwd` + `/etc/shadow` with hashed
passwords, a `/etc/sudoers` policy parser, and a `sudo` that behaves like Linux's
sudo (ticket cache, 3 password tries, NOPASSWD, `-l/-k/-v/-u/-i`, audit log).

### Added
- **Crypto primitives** (`lib/sha256.c`, `include/stitch/sha256.h`): SHA-256
  (FIPS 180-4), HMAC-SHA256 (RFC 2104) and PBKDF2-HMAC-SHA256 (RFC 2898), all
  covered by known-answer self-tests that run at every boot - a broken hash would
  otherwise let every password through.
- **User database** (`kernel/security/userdb.c`): parses `/etc/passwd`,
  `/etc/group` (Linux formats) and `/etc/shadow`, seeding them on first boot with
  `root`/`root` (uid 0) and `user`/`user` (uid 1000, member of `wheel`). Password
  hashes are salted PBKDF2-HMAC-SHA256 stored as
  `$sutureos$pbkdf2-sha256$<iters>$<salt>$<hash>` - deliberately not Linux's
  crypt format, because it is not one.
- **Credentials** (`kernel/security/creds.c`): per-task uid/euid/suid/gid/egid
  (Linux shape). `cred_set_euid(0)` fails for a normal user on purpose; only
  `cred_escalate_root()` - called by sudo after authentication - may raise it.
- **sudo** (`kernel/security/sudo.c`, `sudoers.c`): Linux sudo's contract -
  `sudoers` syntax (`user`/`%group HOST=(RUNAS:GROUP) [NOPASSWD:] CMND`, `Defaults
  timestamp_timeout`/`passwd_tries`), timestamp ticket cache (15 min, `-k`
  invalidates, `-v` refreshes), 3 attempts with `Sorry, try again.` and
  `sudo: 3 incorrect password attempts`, both denial messages, `sudo -l`,
  `sudo -u`, `sudo -i`, and one audit line per invocation in `/var/log/sudo.log`.
- **Shell**: `su [user]`, `whoami`, `id`, `passwd [user]` (two hidden prompts),
  `sudotest` (23-check acceptance), a prompt that shows the identity
  (`sutureos# ` as root, `user@sutureos$ ` otherwise) and real permission checks
  on the privileged commands `mount`/`umount`/`shutdown`/`reboot`/`kill`.

### Fixed
- **ramfs files were capped at 64 bytes** (`uint32_t data[16]`): `/etc/motd` had
  silently been truncated and `/etc/passwd` could not exist at all. Nodes now
  carry `XJ_VFS_MAX_FILE_SIZE` (2048) bytes - this is what made the user database
  possible.
- **Dropped keystrokes at typing speed**: the 8042 has a one-byte output buffer
  and the shell read it once per idle iteration, *after* draining the SMP work
  queue and the NIC ring, so fast input was lost (typing `su user` arrived as
  `user`). A 64-byte input ring is filled with a burst of polls and drained by
  `console_getc()`; the keyboard is now serviced before the other idle work.
- PBKDF2 iterations lowered from 10000 to 2000: 10k cost ~1 s per verification,
  which stalled the shell (and made the password prompt feel broken).

### Verification
- `sudotest`: **23/23 PASS** (hashes, accounts, shadow round-trip, policy,
  ticket, credentials, audit log).
- Interactive QEMU transcript: `su user` -> `user@sutureos$`; `mount ...` denied
  with `permission denied (are you root? try 'sudo mount')`; `sudo -l` lists the
  policy after the password; `sudo mount fat32 0 /mnt/sudomnt` actually mounts and
  logs `RUN user=user runas=root cmd=mount ...`; a second `sudo` runs without a
  password (ticket); `sudo -k` forces the prompt again; 3 wrong passwords produce
  `sudo: 3 incorrect password attempts`; `su root` restores `sutureos#`.
- Regression: self-tests 13/14, FAT32 write+ops PASS, host verifier 17/17,
  AP online, RTL8139 ping, PS/2 mouse - unchanged.

### Known limitations (stated, not hidden)
- This is a **policy layer, not a security boundary**: every task still runs in
  ring 0 and the shell is the only user, so kernel code can bypass sudo. It
  becomes a real boundary when ring-3 userland lands; the cred API already has
  Linux's uid/euid/suid shape so the switch is mechanical.
- Linux sudo itself is a setuid userspace program built on PAM/crypt(3)/termios.
  None of those exist here (no libc in the image, 22 syscalls), so the behaviour
  was ported rather than the binary; password hashing is PBKDF2-HMAC-SHA256
  instead of yescrypt, and there is no secure RNG yet (salts mix the TSC, a
  counter and the user name).
- No `sudoedit`, no PAM modules, no LDAP, no `Defaults` beyond the two above, no
  per-command argument granularity beyond the command name.

## [v0.6.2-alpha]

Keyboard fix release. Typing lost characters (the shell's first keystroke after
boot, and whole words depending on mouse traffic) and the desktop froze a few
seconds after `startgui`, which also made it look like "the keyboard does not
work". Both root causes are documented below.

### Fixed
- **Keyboard starvation (characters/words swallowed)**: the 8042 output buffer
  is shared by keyboard and mouse, and `mbos_kbd_poll()` returned `-1` on an aux
  (mouse) byte *without reading it*. The buffer therefore stayed full and the
  keyboard byte behind it was never read, so typing died until an IRQ1 handler
  happened to consume the aux byte as a scancode. Reproduced on the released
  v0.6.1-alpha: typing `startgui` at the prompt produced no input at all.
  `mbos_kbd_poll()` now drains aux bytes into the mouse decoder
  (`mbos_mouse_feed_byte()`) and returns only scancodes.
- **GUI froze ~4 s after startgui** (frames stopped while `g_tick_count` kept
  advancing, so the desktop looked dead and ignored the keyboard): the frame
  loop called `sched_yield()`, which switches to a stale EEVDF context that
  never comes back. Removed; `gui_pace()` already blocks for one tick.
- **Mouse was initialised twice** (once at boot, again on GUI entry): the second
  `0x20` config-byte read returned a pending data byte (observed `0x9e`) and its
  `0xF6` went unanswered. GUI entry now only queries `mbos_mouse_present()`.
- **`scripts/build.py` / `build_iso.py` hardcoded ROOT** to the main checkout, so
  building from a git worktree silently compiled the wrong sources into the main
  `build/` dir and left stale objects that got linked into a later build (which
  surfaced as an intermittent "hangs after `[SMP] starting AP 1`"). ROOT now
  follows the script location, with `SUTUREOS_ROOT` as an override.

### Added
- `[kbd] key #n sc=.. ch=..` diagnostics (first 12 keys) and a `[gui] frame=..`
  heartbeat every 1000 frames: they separate "the byte never arrived" from "it
  decoded to 0" and "the loop died" from "the timer died" in serial-only runs.

## [v0.6.1-alpha]

Fixup release: the About window was still printing a hardcoded `v0.5.0-alpha`,
so a stale version string was baked into the ISO. Found by the new release
verification, which now also proves the version string is inside the artifact.

### Fixed
- GUI "About SutureOS" reads the version from `include/sutureos/version.h`
  instead of a literal, so the ISO can no longer ship an old version string.

### Added
- `scripts/verify_release.ps1`: proves a release is real - version string inside
  the ISO (and the previous version gone), asset `size`/`digest` read back from
  the API by asset NAME, optional byte-range download-back, every OLD release
  compared against a pre-publish snapshot, and the git side in sync.
- `scripts/publish_all.ps1`: the whole release in one command (snapshot ->
  commit/tag/push -> GitHub Release + ISO -> verification).
- `docs/RELEASING.md` documents the sequence and the checks.

## [v0.6.0-alpha] - unreleased

Kernel hardening on real hypervisors, a much more complete network stack, the
SquashFS filesystem, the boot-presentation rework and a usable GUI pointer.

### Fixed
- **AHCI/SATA on VMware**: the driver treated a lingering `DRQ` bit at command
  completion as a failure (`tf not idle after completion`), which made every
  read fail and left FAT32/ext2/SquashFS unmountable. Completion is now judged
  by `ERR`/`DF` + the fatal `PxIS` bits only, with a bounded settle wait and a
  sticky "device quirk" flag so the wait is paid once.
- **ACPI with a lot of RAM (hang right after `[SMP] BSP LAPIC timer armed`)**:
  with 4 GiB the firmware places the RSDT/MADT above the kernel's identity map
  and the first dereference froze the boot. ACPI structures are now mapped on
  demand (`acpi_touch()`), every length is validated before walking, and the
  MADT subtable walk stops on a zero-length entry instead of spinning.
- Terminal clear actually clears: flanterm does not implement `ESC[2J`, so the
  clear was a no-op; it now erases row by row (`ESC[<row>;1H` + `ESC[2K`), flushes
  the pending double buffer first, and wipes the framebuffer pixels that the
  early fbcon log left behind.

### Added
- **TCP passive open** (`tcp_listen`/`tcp_listen_accept`) plus a one-slot
  out-of-order reassembly buffer, verified by three independent acceptance
  paths (guest as client 7/7 + 6/6, guest as server through QEMU `hostfwd` 8/8,
  and an in-kernel injected-segment self-test `tcpooo`).
- **Socket ABI for `SOCK_STREAM`**: `socket/bind/listen/accept/connect/send/recv/
  shutdown/close` now route to the TCP stack; `tcpapi` echoes through that ABI
  only (8/8).
- **SquashFS 4.0 read-only driver** (gzip data blocks, compressed and raw
  metadata, fragments, id table, fast/extended symlinks), with a shared RFC1951
  inflate extracted from the deb code. Verified by a self-written image
  generator, an independent verifier (83/83) and a host harness that compiles
  the real driver (138/138).
- **Boot presentation**: Limine boots straight into the kernel (no menu), the
  English log scrolls, then the screen is cleared and a SutureOS banner is
  printed before the shell prompt; `startgui` enters the desktop.
- **Console log ring** (128 lines) and `log` / `dmesg` commands that auto-scroll
  to the newest line.
- **PS/2 mouse driver** (8042 aux port, polled), an on-screen arrow pointer, the
  taskbar system name turned into a **start button whose menu pops upwards**
  (Task Manager / Calculator / About SutureOS / Exit to terminal), clickable
  title-bar buttons and client-area clicks forwarded to apps.
- `mouseinfo` diagnostics (8042 status, config byte, device id, packet counts)
  and `startgui test` (pointer self-test through the real packet decoder).
- Host-side harnesses: `vmware_boot.ps1`, `vmware_ui_probe.ps1`,
  `tcp_server_acceptance.py` and a single-sourced version header
  (`include/sutureos/version.h`, also parsed by the ISO manifest).

### Changed
- FAT32 ops self-test now also covers `mkdir`/`rmdir`, `rename` and `unlink`
  (host verifier extended to 17 checks, including "no orphaned cluster chains").

## [v0.5.0-alpha]

Snapshot that the repository started from: GUI framework with Task Manager /
Calculator / About, TCP/IP + UDP/ICMP, several filesystems, SMP bring-up and the
Limine-based ISO. (Tagged retroactively on the commit whose message reads
"完成GUI、TCP/IP、多文件系统等重大更新，升版至 v0.5.0-alpha".)

## [v0.1.0-alpha]

Initial commit: bootable 64-bit kernel with 12/12 self-tests passing.
