# SutureOS v0.70.0-alpha

**ConlyesOS 作者把 virtio-gpu 的「帧缓冲模式」移植给了我们：`-vga none -device virtio-gpu-pci` 下，终端、fbcon 和整个桌面直接画在 virtio 扫描输出上；六个应用图标也由他重绘（扁平面板 + 白色几何字形）。桌面外壳保持原有的圆角 + 轻亚克力 + Fluent 外观不变。**

这一版是**外部贡献合并版**：作者把改好的整棵工作树发过来，我逐字节比对了 87 个差异文件，只并入 virtio-gpu 与图标两部分，外观风格按你的选择保持不变。

---

## ■ 新增

| 能力 | 内容 | 证据 |
|---|---|---|
| **virtio-gpu 帧缓冲模式** | 固件没给线性帧缓冲时（`-vga none`），驱动用 virtio-gpu 的 2D resource 当**系统 framebuffer**：8 MiB 静态 backing（.bss，恒等映射）、`GET_DISPLAY_INFO` → 建 resource → attach → SET_SCANOUT(0) → TRANSFER_TO_HOST + FLUSH；fbcon / flanterm / C 桌面 / Rust 外壳全部照旧画，不需要改自己的代码 | 串口 `[vgpu] framebuffer ready … bpp=32` + `[init] fbcon initialized (virtio-gpu scanout)`；新门禁 15/15 |
| **present 模型** | `virtio_gpu_fb_present/present_rect/mark_dirty/service` 四个入口：ISR 只标 dirty（命令队列永不从中断里发），主循环按 dirty 面积 present；`vgpu_trylock()` 命中忙时跳过一帧而不是死等 | 截图：`screendump` 尺寸 = 上报的 framing 尺寸，banner 与桌面都落在 virtio 输出上 |
| **图标重绘** | 六个应用的图标换成作者画的扁平面板 + 白色几何字形（设置=双滑杆、任务管理器=递增柱、图片=框+太阳+地平线、编辑器=四行文档、Wayland=表面+指针、计算器==号+键盘点）；16 px 标题栏另有一套紧凑字形 | 外观基线重生（动态矩形外 0 像素）；开始菜单/任务栏/标题栏三处同时换成新图标 |
| **几何适配（v0.70.1 修正）** | 我们的 QEMU 11.1 `virtio-gpu-pci` 上报 **1280x800**，而整套渲染器按 **1024x768** 设计（Rust 外壳直接索引固定 1024x768 缓冲）→ 直接采用上报尺寸会让 `startgui` panic（`[gui-slint] PANIC in Rust GUI`）。现在驱动把扫描输出夹到 1024x768，三个 Rust runner 也把 viewport 夹到 1024x768 | `[vgpu] display 1280x800 -> framebuffer 1024x768 (cap 1920x1080)`；门禁截图 1024x768 场景完整渲染 |

## ■ 未合并（作者同棵树里的扁平化桌面）

作者还把这套桌面整体改成「扁平设计」：纯色壁纸、不透明窗面、去掉亚克力/阴影/渐变/伪 3D、任务栏与菜单重绘，并同步写了 `docs/gui-visual-spec.md` 的 v0.70 规范。按你的选择，**这一部分不并入**——`FLAT_*` 色板、扁平壁纸、扁平 chrome 全部没有进入本项目，`draw_app_icon` 只借字形，外壳仍是圆角 + 轻亚克力 + Fluent。理由写进了 `docs/virtio-gpu.md` 的「来源与作者」一节。

## ■ 来源与作者（按项目规则三处登记）

- **作者**：ConlyesOS 作者（交付物内未声明许可；2026-11-25 以整棵工作树的形式提供给我们使用）。
- **登记位置**：`docs/provenance.md`（v0.70 条目 + 合入/未合入范围）、`THIRD_PARTY_LICENSES.md` §21（许可待书面确认，不得声称已获宽松许可）、`docs/licensing-audit.md` §6、`docs/virtio-gpu.md` 末尾的「来源与作者」表、以及 `kernel/drivers/virtio_gpu.c` 文件头注释。
- **比对方式**：87 个差异文件逐字节 + 语义（去行尾/空白）比对，除 virtio-gpu / present 钩子 / 图标 / 文档外没有其它实质改动；交付树里的 `sutureos.img`（作者测试镜像）未入库。

## ■ 验证

**同一份 ISO、一次连续 `scripts/gui_sweep.py` 全量 34 门全绿（34/34 PASS）**：

| 门禁 | 结果 | 门禁 | 结果 |
|---|---|---|---|
| 静态渲染 static | 21/21 | 交互层 interactive | 13/13 |
| 桌面外壳 shell | 55/55 | 外壳行为 shellbehave | 26/26 |
| 交换链 swap | 30/30 | 合成器 damage（oracle 0 mismatch） | 28/28 |
| C 桌面任务栏 apps_taskbar | 9/9 | 编辑器落盘 editorlive | 19/19 |
| 开始菜单 menulaunch | 10/10 | 图片浏览 imgbrowse | 9/9 |
| 计算器 calc | 28/28 | 滚轮 wheel | 9/9 |
| vsync 探测 vsync_present | 17/17 | 键盘归属 keyowners | 13/13 |
| 菜单滚动 menuscroll | 13/13 | 桌面 desktop（图标断言按新规格） | 20/20 |
| 应用内容 apps_content | 23/23 | 外观（新桌面） | 0 像素 |
| 外观（C 桌面） | 0 像素 | Logo logo | 14/14 |
| wl_server | 28/28 | wl_protocol | 29/29 |
| wl_multi | 40/40 | wl_semantics | 38/38 |
| wl_show | 9/9 | wl 桌面窗口 wl_window | 11/11 |
| wlprobe | 14/14 | **virtio-gpu 帧缓冲 vgpufb（新）** | **15/15** |
| GUI 中文 gui_cjk | 4/4 | 图片查看器 imgview | PASS |
| C 编辑器 editor | 5/5 | AF_UNIX unix | 18/18 |
| shm 共享内存 | 18/18 | fd 传递 fdpass | 21/21 |

新门禁 `gui_virtio_gpu_fb_acceptance.py` 的 15 项里最硬的三类证据：
① **无 GOP 启动链路**：`[vga2d] display … scanout=0` → `[vgpu] framebuffer ready: addr=… bpp=32`
→ `[init] fbcon initialized (virtio-gpu scanout)`，且**没有**回退到 "serial console only"；
② **像素落地**：`screendump` 尺寸 = 上报的 framebuffer 尺寸、banner 亮像素、`startgui` 后桌面
在 1024x768 区域有 4000+ chrome 采样点与 200+ 种颜色（不是黑屏）、ESC 回终端；
③ **对照**：`-vga std` 启动**一行 `[vgpu]`/`[vga2d]` 都没有**，老的启动呈现与桌面完全不变
（两套外观门禁各 0 像素越界）。

另外：开机自检 **16/16**；符号扫描 **1464 全局符号 / 0 冲突（145 对象）**。

---

## ■ 产物

| 项 | 值 |
|---|---|
| 版本 | `v0.70.0-alpha` |
| tag | `v0.70.0-alpha`（附注标签，内容见 `build/tag-notes-v0.70.0-alpha.txt`） |
| ISO | `sutureos.iso` **23,881,728 B** |
| sha256 | `9bbf68f058beb34e4d4e496decf30b7ade68d87d5f0a460085f333e44422cc50` |
| 内核 | `kernel.elf` **6,637,536 B** |
| ISO 内版本串 | `0.70.0-alpha` 命中 5 次、`0.69.0-alpha` 命中 0 次 |
| 下载直链 | https://github.com/Fanqi89/SutureOS/releases/download/v0.70.0-alpha/sutureos.iso |
| 发布页 | https://github.com/Fanqi89/SutureOS/releases/tag/v0.70.0-alpha |

---

## ■ 已知限制（照实写）

1. **扫描输出被夹到 1024x768**：宿主显示器比这更大时（QEMU 默认 1280x800），guest 画面显示在左上角、其余为黑。想用满全屏需要把整套渲染器改成尺寸无关（Rust 外壳固定 1024x768 缓冲），本轮没做。
2. 只支持一个扫描输出 / 一个 resource / 一个队列；无 3D、无光标平面、不解析 EDID；32-bpp `B8G8R8A8_UNORM` 一种格式；1920x1080 是静态缓冲上限。
3. 帧缓冲模式与 `vga2d` 演示模式互斥（帧缓冲模式占用时 `vga2d on` 会拒绝启动并如实打印）。
4. present 是**整帧**（或整块 dirty）同步 transfer：每帧 3 MB 在 TCG 下实测约 26 fps（`frame 420 ms=16140`），比 `-vga std` 略慢；没有双缓冲/异步传输。
5. 图标的观感按作者的扁平风格画在我们现有的玻璃 chrome 上——按你的选择保留现有风格，两者搭在一起不是作者原本的整片扁平设计（这是刻意的取舍）。
6. 贡献方**许可条款未书面确认**：在作者确认之前，这份代码只按 GPL-3.0 收录、不得当作可宽松再分发的素材。
7. 全部由 QEMU（11.1）+ TCG 验证，**真机未测**；作者文档里的实机结论基于 QEMU 6.2/1024x768。
