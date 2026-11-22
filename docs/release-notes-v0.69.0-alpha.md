# SutureOS v0.69.0-alpha

**任务栏从"能点"变成"能管"：右键菜单（最大化/还原、最小化、关闭、固定到任务栏）、固定/取消固定实时生效、固定列表写进 FAT32 卷并在下次开机自动读回。**

v0.68 把任务栏补成了"有固定区 + 窗口按钮 + 时钟"，但每个按钮只有**左键**一种语义，固定列表也是编译期常量。这一版补上真正的任务栏管理动作：

---

## ■ 新增

| 能力 | 行为 | 证据 |
|---|---|---|
| **任务栏右键菜单** | 右键窗口按钮弹出 3–4 行菜单（最大化/还原、最小化、关闭、固定到任务栏）；右键固定图标弹出 2 行（打开、取消固定）；左键点行执行，点别处或再次右键关闭 | 日志 `ctx menu open window 5` / `ctx act minimize 5`；门禁断言弹出层像素与壁纸不同（窗口按钮上方 3 行区域） |
| **固定 / 取消固定** | `Shell.pinned` 从编译期常量改成运行时数组（默认 `[0,1,5,3]`，上限 8）；取消固定后**窗口按钮整体左移**（起点 250 → 202），删掉的应用重新出现在开始菜单 | 日志 `pin add` / `pin unpin 3 3`；门禁截图对比固定区像素变化 |
| **固定列表持久化** | 每次变更写 `/mnt/fat/SHELL.CFG`（内容 `pinned=0,1,5\n`）；外壳启动时读回，日志 `taskbar pinned loaded 3 item(s) from SHELL.CFG` | **宿主独立解析 FAT32 镜像**逐字节比对文件内容 + **第二次开机**断言 banner 变成 `3 pinned buttons … window buttons from x=202` |
| **菜单/固定变更的正确性** | 弹出层与固定布局可能改到任意像素 → 这些帧强制整屏重绘（与 v0.68 松手帧同一决策），伤害 oracle 保持 0 mismatch | 合成器门禁 28/28、`damage: identical` 全程 |
| **右键输入通路** | 内核点击队列本来就带按钮号（0=左 1=右 2=中），外壳此前忽略；现在 `shell_step` 的 `click_btn` 参与命中判定 | 门禁用 QMP `mouse_button 2` 真右键驱动 |

### 菜单几何（门禁与实现共用同一套公式）

```
宽 138，行高 30，上下各 4 px 内边距
x = clamp(点击x - 40, 4, 1024 - 宽 - 4)
y = clamp(点击y - 高 - 6, 4, TASKBAR_Y - 高 - 4)   // 永远弹在任务栏上方
行 r 的中心 = (x + 宽/2, y + 4 + r*30 + 15)
```

---

## ■ 验证

**同一份 ISO、一次连续 `scripts/gui_sweep.py` 全量 33 门全绿（33/33 PASS）**：

| 门禁 | 结果 | 门禁 | 结果 |
|---|---|---|---|
| 静态渲染 static | 21/21 | 交互层 interactive | 13/13 |
| 桌面外壳 shell | 55/55 | **外壳行为 shellbehave（含右键菜单/固定持久化）** | **26/26** |
| 交换链 swap | 30/30 | 合成器 damage（oracle 0 mismatch） | 28/28 |
| C 桌面任务栏 apps_taskbar | 9/9 | 编辑器落盘 editorlive | 19/19 |
| 开始菜单 menulaunch | 10/10 | 图片浏览 imgbrowse | 9/9 |
| 计算器 calc | 28/28 | 滚轮 wheel | 9/9 |
| vsync 探测 vsync_present | 17/17 | 键盘归属 keyowners | 13/13 |
| 菜单滚动 menuscroll | 13/13 | 桌面 desktop | 19/19 |
| 应用内容 apps_content | 23/23 | 外观（新桌面） | 0 像素 |
| 外观（C 桌面） | 0 像素 | Logo logo | 14/14 |
| wl_server | 28/28 | wl_protocol | 29/29 |
| wl_multi | 40/40 | wl_semantics | 38/38 |
| wl_show | 9/9 | wl 桌面窗口 wl_window | 11/11 |
| wlprobe | 14/14 | GUI 中文 gui_cjk | 4/4 |
| 图片查看器 imgview | PASS | C 编辑器 editor | 5/5 |
| AF_UNIX unix | 18/18 | shm 共享内存 | 18/18 |
| fd 传递 fdpass | 21/21 | | |

新门禁从 16 项扩到 **26 项**，新增的 10 项里最硬的两条是：
- **宿主解析 FAT32 镜像**读出 `SHELL.CFG`，断言内容恰好是 `pinned=0,1,5\n`（不是"日志说写了"，而是盘上真有）；
- **第二次开机**（同一块盘、不重建夹具）断言 `taskbar pinned loaded 3 item(s)` 且窗口按钮起点 202 —— 持久化闭环。

另外：开机自检 **16/16**；符号扫描 **1453 全局符号 / 0 冲突（145 对象）**。

### 顺带修掉一个"假失败"来源

首次全量 sweep 有两门塌成 `2/29`、`23/28`，排查发现是**上一门超时退出时把 QEMU 留在了后台**：僵尸 VM
占着一颗核（2 核实机），下一门自然时序全乱。`scripts/gui_sweep.py` 现在每门结束后按 pid 清理"本门新起
的 QEMU"（只杀自己在这一门里 spawn 的进程，不碰别人的 VM，并打印 `[sweep] killed orphan qemu pid=N`）；
清理后连续全量一次 **33/33**。

---

## ■ 产物

| 项 | 值 |
|---|---|
| 版本 | `v0.69.0-alpha` |
| tag | `v0.69.0-alpha`（附注标签，内容见 `build/tag-notes-v0.69.0-alpha.txt`） |
| ISO | `sutureos.iso` **23,865,344 B** |
| sha256 | `649f406b0ab0984901010a11dac9abc911475a5bfde26d6ade61a8b0e4ef3701` |
| 内核 | `kernel.elf` **6,627,376 B** |
| ISO 内版本串 | `0.69.0-alpha` 命中 5 次、`0.68.0-alpha` 命中 0 次 |
| 下载直链 | https://github.com/Fanqi89/SutureOS/releases/download/v0.69.0-alpha/sutureos.iso |
| 发布页 | https://github.com/Fanqi89/SutureOS/releases/tag/v0.69.0-alpha |

---

## ■ 已知限制（照实写）

1. **菜单是固定 4 行模型**，没有子菜单、勾选态、快捷键提示；不支持键盘导航（ESC 由外壳退出键处理，菜单内 ↑/↓ 不移动）。
2. **固定上限 8**（`PIN_MAX`），且固定顺序只在"右上角追加"，不能在固定区内部拖动排序。
3. 持久化只到这个**测试卷的 `/mnt/fat/SHELL.CFG`**：装到真盘（安装器 + PERSIST 卷）时该路径不一定存在，写失败会如实打 `rc<0`，此时固定列表退回默认值（不静默假装成功）。
4. 右键菜单坐标靠 `TASKBAR_Y` 计算，任务栏高度若改变需要同步（当前 38 px）。
5. 菜单打开期间关闭/最小化动画与菜单同时存在时，菜单固定在最上层（不做遮挡裁剪）。
6. 仍是 QEMU 截图/日志/宿主镜像三方证明，**真机未验证**；右键在真实 PS/2 三键鼠标上未实测（QEMU 的 PS/2 模型支持右键）。
