# Slint GUI 第一帧（Stage 0/1 原型）交付说明

- **日期**：2026-11-03
- **分支/worktree**：`port/suture-gui` @ `build/port-suture-gui`，基线 `main@ebb1897`（Rust core 已进内核那版）
- **主仓**：未改动、未 commit/tag/push；交付形式是一个 patch
- **一句话结论**：**第一帧已经在 QEMU 里真实画出来了**（13/13 像素断言 PASS），但**渲染器是手写的 Rust 软件光栅器，不是 Slint** —— Slint 在内核里"能链接、能建平台、能建组件、timer 能跑"，但 `SoftwareRenderer::render()` 在"矩形≥2 层 + 渐变/阴影"的场景里**不返回**（最小场景能画），按约定时间盒降级。

---

## 1. 交付物

### 新增

| 文件 | 行数 | 作用 |
|------|------|------|
| `rust/suture-gui/Cargo.toml` | 34 | staticlib；`default=[]` 手写光栅器，`slint` 特性 = Slint 后端 |
| `rust/suture-gui/build.rs` | 18 | 只有开了 `slint` 特性才编译 `.slint`（`EmbedForSoftwareRenderer`） |
| `rust/suture-gui/src/lib.rs` | 360 | 场景绘制 + C ABI（`suture_gui_version/renderer/selftest_render/frames`）+ 可选 Slint 后端 |
| `rust/suture-gui/src/swrast.rs` | 354 | 手写软光栅器：圆角矩形（解析 AA）、90°/180° 渐变、alpha 合成、4 层假阴影、伪 3D 图标 |
| `rust/suture-gui/ui/main_v2.slint` | 128 | Slint 版场景（`--features slint` 时编译；本轮定位问题用） |
| `rust/suture-gui/ui/main_min.slint` | 14 | 二分用最小场景（**能画出来**，证明渲染器本身可用） |
| `rust/suture-gui/ui/main_nottext.slint` | 139 | 二分：去掉 Text 的版本 |
| `rust/suture-gui/ui/main_full.slint` | 168 | 最初完整设计（含 Text/圆形色球） |
| `kernel/gui/slint_bridge.c` | 75 | C 侧边界：weak 回退、framebuffer 探测、`[gui-slint]` 日志行 |
| `kernel/gui/slint_bridge.h` | 35 | 对外只有 `slint_gui_render_frame()` / `slint_gui_linked()` |
| `scripts/slint_gui_acceptance.py` | 272 | QEMU + QMP + screendump + 13 项像素/日志断言 |

### 修改

| 文件 | 改动 | 说明 |
|------|------|------|
| `scripts/build.py` | +89/-2 | 新增 Rust GUI 静态库构建：**提取归档里全部对象**（含 compiler_builtins 的软浮点 intrinsic），跳过非 `.o`；**用 `@response file` 传 400+ 个对象**（Windows 命令行上限 32767 字符，否则 `WinError 206`） |
| `kernel/shell/shell.c` | +56 | `guislint` 命令：暂停终端绘制 → 渲一帧 → 保持 10 s（ESC 退出）→ 还给终端 |
| `kernel/shell/shell.h` | +3 | `shell_cmd_guislint` 原型 |

**total: +146/-2 in 3 tracked files, +9 new files**

---

## 2. 构建结果（实测）

```
$ py -3 scripts/build.py
Compiled 116 source files + trampoline + font + user blob
Building Rust core (x86_64-unknown-none)...
Rust core: libsuture_core.a (7423206 bytes)
Building Rust GUI stack (Slint, x86_64-unknown-none)...
Rust GUI: libsuture_gui.a (...)
Rust GUI objects: 417 extracted (soft-float intrinsics included)
Linking...
link response file: link.rsp (568 objects)
BUILD OK -> build/kernel.elf
kernel.elf: 4956696 bytes     # 基线 2 712 512 → +2.24 MB（其中光栅器本体 ~30 KB，其余是 libm/libcore/linkshim）
```

> kernel.elf 从 2.71 MB 涨到 4.96 MB：本轮链接的是 libcore+libm+compiler_builtins 全量对象（未做 `--gc-sections`）。**下一批应加 `-Wl,--gc-sections`**，预计可回落到 +0.2 MB 量级。
> 之前 Slint 版本链接后是 **7.25 MB**（+4.55 MB），这也是不选它的原因之一。

ISO：`build/sutureos.iso` 25.1 MB（基线 25.0 MB 左右）。

---

## 3. 验收结果（`scripts/slint_gui_acceptance.py`）

`checks=13 ok=13 fail=0` — **PASS**

| # | 断言 | 实测 |
|---|------|------|
| 1 | 串口出现 `[gui-slint] frame rendered` | `[gui-slint] frame rendered 1024x768 checksum=917e8f72 frames=1 OK` |
| 2 | 无 PANIC | 日志无 `PANIC` |
| 3 | 命令正常还回终端 | `guislint: frame shown, back to terminal` |
| 4 | 转储尺寸 | 1024×768 |
| 5-8 | 四个圆角像素 ≠ 面板填充 | `|d|=85 / 91 / 68 / 107`（圆角真被裁掉，且不受背景色影响） |
| 9 | 半透明：同一面板压在两种背景上不同 | 蓝紫球 (100,99,186) vs 底色 (45,49,58)，`|d|=233` |
| 10 | 假阴影：越远越接近背景 | 亮度 80.0 → 103.5 → 117.6（y=493/502/514） |
| 11 | 伪 3D 图标最亮行在顶部 45% | 第 5/72 行（223.7 vs 最暗 73.3） |
| 12 | 图标上下亮度对比 | 150.4（阈值 20） |
| 13 | 高光比面板亮 | 223.7 vs 125.0 |

**截图（给用户看的第一帧）**：
- `build/slint_gui.png`（1024×768，35 KB）
- `build/slint_gui.ppm`（原始转储）

画面内容：深色渐变壁纸 + 两个圆角柔光色块 + 一块**半透明圆角面板**（能看到右侧青蓝球透过面板）+ 4 层叠加的柔和投影 + **伪 3D 玻璃质感图标**（渐变体 + 顶部高光带 + 底部内阴影 + 中央白色 glyph 板）+ 三层微透叠层 + 两张列表行 + 右侧次级卡片。

---

## 4. Slint 到底卡在哪（本轮最重要的负面结论）

### 4.1 已经走通的部分（都是硬证据）

| 阶段 | 证据 |
|------|------|
| 静态库编到 `x86_64-unknown-none` | `cargo build` Finished；归档 448 成员（Slint 全树） |
| 链接进 kernel.elf | 首次链接报 `undefined reference to __addsf3/__mulsf3/__gtsf2/__fixsfsi…`（目标无 SSE → 软浮点）→ 把 **compiler_builtins 一起提取**后通过（其 `memcpy/memset` 是 **W 弱符号**，kernel/lib/string.c 的 **T 强符号**胜出，不冲突） |
| 400+ 对象传参 | `WinError 206 文件名或扩展名太长` → 改 `@link.rsp` 响应文件 |
| 内核里真跑起来 | 日志 `[gui-slint] platform ready (slint software renderer)` |
| allocator 桥 | `lemis_heap_alloc/free` + 16 字节对齐（超对齐走 backref），组件创建成功即证明分配可用 |
| 时间源 | `g_tick_count`（volatile 读）→ `duration_since_start`；`update_timers_and_animations()` 返回 |

### 4.2 卡死点：`SoftwareRenderer::render()` 不返回

加了分阶段日志（`stage: timers` / `stage: draw` / `stage: draw done`）后，三种场景的结果：

| 场景 | 结果 |
|------|------|
| **A. 最小**：窗口底色 + 1 个圆角矩形 | ✅ `stage: draw done` → `frame rendered … checksum=95663a01 OK` |
| **B. 去 Text 的完整版**（渐变壁纸 + 两个圆形色球 + 4 层阴影 + 图标） | ❌ 停在 `stage: draw`，**无 panic、无返回**（QMP 仍连着 → 是死循环/极慢，不是崩溃） |
| **C. 完整设计（含 Text）** | ❌ 更早的版本在 `i-slint-core-1.18.1/item_tree.rs:1414` panic（`item_tree_array[idx]` 越界）——**Text 元素会踩到 Slint 的 item tree 索引问题** |

所以卡点不是 alloc、不是字体、不是链接，而是 **软件渲染器在"多图元 + 渐变/阴影"的填充路径里不收敛**；怀疑与我们这套 framebuffer/stride 组合下的 span/dirty 处理或软浮点路径有关（`render()` 内部对渐变做 float 角度运算，f32 软浮点极慢也可能是"假死"，但 40 s 未返回已被验收脚本判定为卡死）。

**这一条不是"再调一下参数"能解决的**，需要下一步在宿主上用同一份输入复现 + 打点（`i-slint-renderer-software` 的 `process_rectangle_impl` 里已有渐变分支，可加计数）。因此本轮按 Lead 给的降级方案交付。

### 4.3 保留在树里的 Slint 后端

`cargo build --features slint` 可以完整复现（同样的 C ABI、同样的 `guislint` 命令），方便下一批接着修。默认构建不依赖 Slint，所以 CI/构建速度不受影响（默认 crate 只依赖 `libm`）。

---

## 5. 已知限制（如实）

1. **没有文字**。Slint 的 Text 会 panic（见 4.2 C）；手写光栅器本轮**没有实现字形渲染**。文字要等：① Slint 修好；或 ② 把 `kernel/term/cjk16.c` 的 16×16 GB2312 点阵接到光栅器（这条路更稳，能直接复用已有中文字库）。
2. **单帧静态**，没有输入、没有动效、没有窗口管理；`guislint` 渲完就停住等你按键。
3. **两个彩色色块是"探针"不是最终美术**：它们存在的唯一目的是让"半透明"可被像素断言（同一面板压在不同背景上必须不同）。正式视觉应该是壁纸/ imagery。
4. **没有模糊（真亚克力）**：本轮只做了"半透明 + 微透叠层 + 顶部高光"，没有任何背景模糊。真亚克力需要离屏 box-blur（可行性文档 §4.4 有方案，成本 3~6 ms/帧或零成本静态壁纸模糊）。
5. **体积**：kernel.elf +2.24 MB（未开 `--gc-sections`，下一批必须开）。
6. **性能未测**：单帧 1024×768 软光栅（圆角 AA + 渐变）在 QEMU 上从命令到日志约 1 s 内，但没有 fps 数据；动效前必须先测。
7. **24bpp framebuffer 未支持**（只做 32bpp；`slint_bridge.c` 会如实报 `need a 32bpp framebuffer`）。
8. **没有改任何现有 GUI**：`kernel/gui/gui.c`、`kernel/gui/apps.c`、开始菜单、任务栏、启动呈现**零改动**；`guislint` 是新增命令，跑完 `term_clear()` 回到终端。

---

## 6. 回归：现有桌面没被动

- `git diff --stat`：只碰 `kernel/shell/shell.c`(+56)、`shell.h`(+3)、`scripts/build.py`(+89/-2)；`kernel/gui/*`、`kernel/term/*`、`fbcon.c` **一行没动**。
- `scripts/apps_taskbar_acceptance.py`：**9/9 PASS**（注册表菜单 8 项、最大化、还原、`n` 最小化、任务栏按钮 restore↔minimize↔restore、固定应用、Win 键、任务栏带 8 色）。
- 内核自检行数不变（原 13/14），`[rust] core … PASS` 仍在。

---

## 7. 下一批建议（按优先级）

1. **接中文字形**：复用 `kernel/term/cjk16.c`（7443 字形 16×16 点阵，已有），在 `swrast` 里加 `draw_glyph()`；这是"新风格真的有字"的最短路径，且不需要修复 Slint。
2. **加 `--gc-sections`**：把 kernel.elf 的 +2.24 MB 压回去（`-ffunction-sections -fdata-sections` + ld `--gc-sections`，注意 weak 回退对象要保留）。
3. **性能与动效**：`guislint` 改成循环（`update_timers` + 重绘脏区），量 fps；先做窗口打开/最小化的位移+透明度动画。
4. **真亚克力**：静态模糊壁纸（零成本）+ 面板用模糊壁纸当底；需要一次离屏 buffer（1024×768×4 = 3 MB `.bss`）。
5. **回头修 Slint 的 render 不返回**：在宿主复现（同一份 `.slint` + 同一 stride），二分到具体图元属性；如果只是渐变，可以先"Slint 只做静态布局、光栅器仍自绘"。
6. 之后才是合成器/Wayland（可行性文档结论：需要 AF_UNIX + SCM_RIGHTS + epoll + memfd + Rust std，一个季度起）。

---

## 8. 复现命令

```powershell
cd C:\Users\fanqi\Desktop\SutureOS\build\port-suture-gui
$env:PATH = "$env:USERPROFILE\.cargo\bin;$env:PATH"     # 必须用 rustup 的 cargo（MSYS2 版没有 none target 的 liballoc）
py -3 scripts\build.py                                  # C + Rust core + Rust GUI
py -3 scripts\build_iso.py
py -3 scripts\slint_gui_acceptance.py                   # QEMU → guislint → screendump → 13 项断言

# Slint 后端（复现"能初始化但 render 不返回"）
cd rust\suture-gui; cargo build --release --target x86_64-unknown-none --features slint
```
