# SutureOS v0.63.0-alpha

**桌面外壳继续做实：文本编辑器变成真编辑（缓冲区 + 光标 + F5 落盘）、图片查看器读真实文件并支持
BMP/QOI 双解码与左右切换、计算器成为第 6 个应用；同时把 damage 合并改成 32×32 tile 网格、
把 Wayland 侧补到完整 US keymap / repeat_info / memfd 引用计数，并修掉几个只会"看起来对"的真 bug。**

本版接在 v0.59.0-alpha 之后（0.60–0.62 的内部批次合并进这一个发布），启动方式与启动呈现完全未变：
滚动英文日志 → 系统名 banner → 黑底白字终端 → 显式输入 `startgui` 进桌面。桌面视觉基线按"关于本机"
版本号的变化重新冻结，外观门禁仍然 0 像素。

---

## ■ 关键修复

| 现象 | 根因 | 修复 |
|---|---|---|
| 任务管理器的心跳点/数字出现在**其它窗口里面**（叠在上层的编辑器窗口里能看到青色圆点） | `compose_rect()` 先合成所有窗口层、最后统一绘制"活数据"，活数据因此永远在最上层 | 每个窗口合成完自己的层后立刻画自己的活数据，上层窗口自然覆盖；验收加"该处 cyan 像素必须为 0"断言 |
| 编辑器里打字"打进去了但屏幕上看不到"（`editor saved 46 bytes` 而不是 51） | `ed_shift_right()` 在 `from >= len` 时提前返回，光标在行尾时字符写进缓冲区但长度不增 | 允许 `from == len`（行尾追加） |
| 桌面的 Wayland 窗口永远收不到指针事件（`desktop pump` 一行日志都没有） | ①`wl_server_pump()` 读到 EOF 只 `break`、连接槽 `used` 永远为 1（4 槽耗尽且"有活连接就 return"永久早退）；②本内核 ring-3 退出后 unix 对端不报 EOF，槽看着仍活 | ①新增 `conn_release()`（EOF 后先 dispatch 再清槽，保留 `g_keep[]` surface）；②改为按"谁拥有输入设备"判定：`source=shell` 时单任务内核没有客户端能 pump，桌面 pump 必须跑 |
| 串口日志里出现 `image: loaded IMG.BMP进程与性能CPU内存…`（整段桌面文字） | `console_printf("%s", &str.as_ptr())`：`&str` 没有 NUL 结尾，`%s` 一路读到后面内存 | 另存 NUL 结尾的字节串给 `%s` |
| 计算器算 `7-10` 时日志是 `4294967293` | 内核 `console_printf` 的 `%d` 按无符号读 | 负数单独打 `-` + 绝对值 |
| 计算器窗口里是通用占位（"SutureOS 应用内容"），键盘与日志都对但屏幕上看不到键盘板 | `draw_body()` 的 accent 分派漏了 `ACC_CALC` | 补分派；验收用键盘板像素计数（23582 px）抓住这类"看不见的应用" |
| 计算器键盘板上压着一行"输入: 键入文本 Tab 切换窗口" | `draw_focus_line()` 的跳过列表只排除了图片/编辑器/Wayland 窗口，计算器自己也吃键盘却没排除 | 跳过列表加窗口 5 |
| 多热点的帧退化成整屏重绘（damage 最多 6 块、满了退包围盒） | 旧 `Damage` 的合并策略 | 改为 32×32 tile 位图（32×24）+ 行扫描纵向合并重建矩形（上限 16 块）；光标快路径判据改成 `tile_count() <= 4`（tile 粒度下斜向移动会触 4 tile） |
| 开始菜单几何被"4 行/236 px"冻结，加应用就移动全部坐标 | 菜单高度/原点/行距是常量 | 改成 `menu_h() = 46 + 项数×44 + 14` / `menu_y()` / `menu_items_y()` / `menu_rect()` 四个函数，绘制/命中/伤害统一走它们 |

## ■ 新增能力

**桌面应用**
- **文本编辑器真编辑**：10×38 多行缓冲区，可打印字符 / Backspace（跨行合并）/ Enter（拆行）/
  方向键 / Home / End，行号 + 当前行高亮 + 2px 光标 + `Ln x, Col y ・ N 行 ・ F5 保存`；
  **F5 经内核 VFS 写 `/mnt/fat/GUIEDIT.TXT`**。
- **图片查看器读真实文件**：Rust 侧 **BMP（24/32bpp）与 QOI（RGB/RGBA 全 opcode）双解码**，
  `vfs_read_file()` 读 `/mnt/fat/IMG.BMP` / `IMG.QOI`，整数最近邻缩放，页脚写实际文件名/尺寸/倍数；
  **窗口持有焦点时 ←/→ 切换文件**。Wayland surface 仍优先于两者。
- **计算器（第 6 个应用）**：整数四则（i64 饱和）、数字 / `+ - * /` / `=` / `c` / Backspace，
  除零进错误态；显示条 2× 当前值 + 4×4 键盘板（待定运算符按键用强调色高亮）。
- **开始菜单 6 行**：第 5 行打开 Wayland 窗口（以前只能按 F2）、第 6 行计算器；面板高度随应用数增长。
- **窗口表 5→6 收成常量**：`const WINS: usize = 6`，替换了数组/循环/边界/签名里的全部字面量 5。

**合成器**
- damage 合并改为 **32×32 tile 网格**（`damage: identical` oracle 仍 0 mismatch；安静窗口
  ≈250 tiles / 4 块矩形 / permille 132）。
- 每个窗口的活数据与自己的层一起合成（修掉穿透）。

**Wayland**
- 窗口摆放语义：指针事件按窗口矩形换算成 surface 局部坐标（`local = (mouse - origin) / scale`
  后夹到逻辑尺寸），enter 以窗口矩形为界，键盘按窗口焦点门控。
- **完整 US keymap 3936 B**（数字行 + 全部 Shift 符号），服务端/客户端/宿主三方 FNV `48c3b318`；
  `wl_keyboard.repeat_info(rate=30, delay=400)`。
- **memfd 引用计数**：`memfd_ref/unref/live_count`，pool/buffer/连接三个释放点归零才关，
  实测 live 序列 3→2→2→1→0；buffer/pool 生命周期语义（release 每 attach 周期一次、
  destroy 后不可 attach、pool destroy 后 create_buffer 被拒）。

**验收脚本（新增 3 个门禁）**
- `scripts/gui_editor_live_acceptance.py`（12/12）：真键盘输入 → F5 → **宿主解析 FAT32 镜像逐字节比对**。
- `scripts/gui_menu_launch_acceptance.py`（10/10）：菜单面板边缘像素、每行 accent 图标、点第 5 行
  打开 Wayland 窗口、F2 minimize→restore。
- `scripts/gui_image_browse_acceptance.py`（9/9）：BMP 四象限 → → QOI 四象限 → caption 跟随 → ← 回 BMP。
- `scripts/gui_calc_acceptance.py`（10/10）：`12+30=`→42、`9*9=`→81、`7-10=`→-3、`8/0=`→错误、
  `c` 清空、显示条与键盘板像素。

## ■ 验证

同一份最终 ISO 真跑（QEMU，一次连续 `scripts/gui_sweep.py` 全量 **28 门全绿**）：

```
static        21/21     interactive   13/13     shell         55/55
swap          30/30     damage        28/28     apps_taskbar   9/9
editorlive    12/12     menulaunch    10/10     imgbrowse      9/9
calc          10/10     desktop       19/19     apps_content  23/23
appearance_new 0 px     appearance_classic 0 px  logo         14/14
wl_server     28/28     wl_protocol   29/29     wl_multi      40/40
wl_semantics  38/38     wl_show        9/9      wl_window     11/11
wlprobe       14/14     gui_cjk        4/4      imgview        PASS
editor         5/5      unix          18/18     shm           18/18
fdpass        21/21     === 28/28 PASS ===
```

另外：开机自检 **16/16**；符号扫描 **1435 全局符号 / 0 冲突（144 对象）**；
ISO 内新版本串 `0.63.0-alpha` 命中 5 次、旧串 `0.59.0-alpha` 命中 **0 次**；
GUI 外观门禁（新桌面）**0 像素**（基线已按"关于本机"版本号重新冻结）。

## ■ 产物

| 项 | 值 |
|---|---|
| 文件名 | `sutureos.iso` |
| 字节数 | 23,742,464 |
| sha256 | `31ed2f53a9f8aee324af4949f4bfedfe211aa866ef1dce1ddefeba4e62039dcc` |
| 下载直链 | https://github.com/Fanqi89/SutureOS/releases/download/v0.63.0-alpha/sutureos.iso |
| 发布页 | https://github.com/Fanqi89/SutureOS/releases/tag/v0.63.0-alpha |

## ■ 已知限制

- 桌面是 **ring-0 的 Rust 外壳**，不是 ring-3 合成器；60 Hz 是 HPET 栅格而不是真 vsync，
  `dropped` 是软件推断；无 scanline 中断。
- 编辑器：10 行 × 38 列、仅 ASCII、无选中/撤销/滚动，F5 固定写 `GUIEDIT.TXT`。
- 图片查看器：只支持 BMP（24/32bpp）与 QOI；≤256×256，超出回退 Logo；不支持 PNG/JPEG；
  候选文件是写死的两个路径，没有文件选择器。
- 计算器：只有整数（无小数/百分比/括号/内存），无历史记录。
- 菜单 6 行已接近面板高度上限，再加行需要分栏或滚动。
- Wayland：桌面 pump 只**记录**不写 socket（本内核单任务外壳，`startgui` 期间跑不了 ring-3 客户端），
  死槽回收依赖"单任务内核"假设；keymap 只发送不解释（无 XKB 引擎）、repeat_info 只告知；
  关闭但仍被映射的 memfd 不归还物理页（上限 4 对象 × 64 页）。
- 外观冻结规则仍然有效：启动呈现与桌面风格由 SutureOS 自绘，不引入外部桌面环境。
