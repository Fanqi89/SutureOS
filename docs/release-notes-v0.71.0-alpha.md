# SutureOS v0.71.0-alpha

**外观风格现在可以切换了：系统设置 → 外观风格，点一下在「玻璃 · Fluent」和「扁平 · Flat」之间来回切，选择写进 SHELL.CFG，重启后保留。** 为了这一行能点，系统设置窗口也从纯展示变成了第一个真正可交互的设置面板。

![同一台机器、同一个设置面板：玻璃风格（默认）](C:/Users/fanqi/Desktop/SutureOS/docs/screenshots/gui-v071-style-glass.png)

![同一台机器、同一个设置面板：扁平风格（点一下切换）](C:/Users/fanqi/Desktop/SutureOS/docs/screenshots/gui-v071-style-flat.png)

---

## ■ 新增

| 能力 | 内容 | 证据 |
|---|---|---|
| **两种外观风格** | `STYLE_GLASS`（默认，v0.37–v0.70 的渐变壁纸 + 亚克力窗体 + 柔和阴影 + 半透明任务栏/菜单）与 `STYLE_FLAT`（ConlyesOS 作者的扁平语言：纯色壁纸、不透明面板、1 px 发丝线、无阴影无模糊） | 新门禁 `gui_style_switch_acceptance.py` **14/14**：玻璃壁纸有垂直渐变（通道差 >12）、扁平壁纸是一种纯色（差 ≤3）、窗体标题条/任务栏取到扁平色板的精确色值 |
| **系统设置变成可交互面板** | `系统设置` 第二行改成 `外观风格`，值显示当前风格并带「点击切换」；点击命中由 `apps::settings_row_at()` 按 `settings_body` 同一套布局常量判定（`ROW_TOP=TITLE_H+20`、`ROW_H=52`、`ROW_PITCH=62`） | 点一下 → 日志 `[gui-shell] style: flat` + `shell cfg saved: pinned=4 style=1`；再点 → `style: glass (fluent)` + `style=0` |
| **风格持久化** | 与固定列表共用 `/mnt/fat/SHELL.CFG`：新增第二行 `style=flat|glass`，启动 `taskbar_load()` 解析并重建壁纸缓存 | **同一块盘第二次开机**断言 `shell cfg loaded: pinned=4 style=1`，且桌面直接以扁平风格启动 |
| **切换是"重画"而不是"混着画"** | 风格进入窗口层缓存键（`layer_key` 混入 style），切换时重建壁纸/磨砂缓存（`build_caches`）并强制整屏帧；设置行自身也重新栅格化 | 切换后帧稳定（门禁连续两次 `screendump` 逐字节相同才取样），窗口内容、任务管理器条、时钟全部正确重绘 |

## ■ 实现要点（三个真实修掉的问题）

1. **扁平窗体的应用内容丢了**：`draw_window` 的内容绘制在玻璃路径末尾，扁平路径 `return` 太早 → 扁平模式下窗口只有标题栏、正文空白。修法：扁平路径末尾同样调用 `apps::draw_body`。
2. **设置行上的"输入: 键入文本"横条**：`draw_focus_line` 在 `w.y+TITLE_H+104` 处画输入提示，正好压在新的 `外观风格` 行上（截图里两层文字重叠）。修法：系统设置窗口不再画输入条（它是点击驱动、不是文本框）。
3. **切换后行值不更新 / 壁纸残留**：`settings_set_style()` 只在帧首调用，而配置是在同一帧稍后才读进 Shell 的 → 缓存层里保留旧文案；壁纸缓存是开机建一次的静态缓存，切换时未重建。修法：读配置后与切换时都同步文案并失效该窗口层；`set_style()` 里 `unsafe { build_caches(); }` 重建壁纸/磨砂源。

## ■ 验证

**同一份 ISO、一次连续 `scripts/gui_sweep.py` 全量 35 门全绿（35/35 PASS）**：

| 门禁 | 结果 | 门禁 | 结果 |
|---|---|---|---|
| 静态渲染 static | 21/21 | 交互层 interactive | 13/13 |
| 桌面外壳 shell | 55/55 | 外壳行为 shellbehave（含 SHELL.CFG 两行格式） | 26/26 |
| 交换链 swap | 30/30 | 合成器 damage（oracle 0 mismatch） | 28/28 |
| C 桌面任务栏 apps_taskbar | 9/9 | 编辑器落盘 editorlive | 19/19 |
| 开始菜单 menulaunch | 10/10 | 图片浏览 imgbrowse | 9/9 |
| 计算器 calc | 28/28 | 滚轮 wheel | 9/9 |
| vsync 探测 vsync_present | 17/17 | 键盘归属 keyowners | 13/13 |
| 菜单滚动 menuscroll | 13/13 | 桌面 desktop | 20/20 |
| 应用内容 apps_content | 23/23 | 外观（新桌面，玻璃默认） | 0 像素 |
| 外观（C 桌面） | 0 像素 | Logo logo | 14/14 |
| wl_server | 28/28 | wl_protocol | 29/29 |
| wl_multi | 40/40 | wl_semantics | 38/38 |
| wl_show | 9/9 | wl 桌面窗口 wl_window | 11/11 |
| wlprobe | 14/14 | virtio-gpu 帧缓冲 vgpufb | 15/15 |
| **外观风格切换 style（新）** | **14/14** | GUI 中文 gui_cjk | 4/4 |
| 图片查看器 imgview | PASS | C 编辑器 editor | 5/5 |
| AF_UNIX unix | 18/18 | shm 共享内存 | 18/18 |
| fd 传递 fdpass | 21/21 | | |

新门禁 `gui_style_switch_acceptance.py` 的 14 项里最硬的四类证据：
① **默认是玻璃**：壁纸上下采样点通道和 >12（真有渐变），配置日志 `style=0`；
② **点击切到扁平**：`[gui-shell] style: flat` + `shell cfg saved: pinned=4 style=1`，
壁纸变成一种纯色（通道和 ≤3），标题条取到扁平标题色 `0x1f2734`、任务栏取到 `0x0b1018`，
并且**窗口正文仍在绘制**（系统设置正文 >300 个亮像素——这条是补上"扁平窗口空白"那个 bug 的回归检查）；
③ **再点回玻璃**：`style: glass (fluent)` + 渐变恢复；
④ **持久化**：同一块盘第二次开机断言 `shell cfg loaded: pinned=4 style=1` 且桌面直接以扁平启动。

另外：开机自检 **16/16**；符号扫描 **1464 全局 / 0 冲突（145 对象）**。

---

## ■ 产物

| 项 | 值 |
|---|---|
| 版本 | `v0.71.0-alpha` |
| tag | `v0.71.0-alpha`（附注标签，内容见 `build/tag-notes-v0.71.0-alpha.txt`） |
| ISO | `sutureos.iso` **23,885,824 B** |
| sha256 | `0b5bafe7cb4913f238ad10af9c39886e980fbf8e25e770795e362ea5f1062f20` |
| 内核 | `kernel.elf` **6,638,032 B** |
| ISO 内版本串 | `0.71.0-alpha` 命中 5 次、`0.70.0-alpha` 命中 0 次 |
| 下载直链 | https://github.com/Fanqi89/SutureOS/releases/download/v0.71.0-alpha/sutureos.iso |
| 发布页 | https://github.com/Fanqi89/SutureOS/releases/tag/v0.71.0-alpha |

---

## ■ 已知限制（照实写）

1. **只有两种风格**，且切换是"整片换皮"：每种风格是一套固定的绘制分支，不支持自定义配色/圆角半径/字体。
2. 切换瞬间要重建壁纸缓存（渐变 + 两个色块 + 256×192 镜面 box-blur），QEMU TCG 下这一帧要 2–4 秒；门禁为此等到画面稳定才取样。真机上应远低于此，但未在真机测。
3. 设置面板目前**只有这一行可点**：账户/关于仍是只读展示，没有输入框、开关、滚动；点击只在窗口正文内、且只在系统设置窗口生效。
4. 扁平风格是"按作者的色板重画我们的控件"，不是他那棵树的逐像素复刻：字体仍是我们自己的点阵字体，任务栏/菜单/右键菜单的布局与交互沿用我们的实现。
5. `style` 与 `pinned` 同存 `/mnt/fat/SHELL.CFG`；装到真盘（安装器 + PERSIST 卷）时该路径不一定存在，写失败会如实打 `rc<0` 并只在本次会话生效。
6. 外观基线仍钉在**玻璃（默认）**风格；扁平风格没有基线门禁，只有像素色值断言。
7. 全部 QEMU（11.1）+ TCG 验证，**真机未测**。
