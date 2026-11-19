# Slint 路线结论（v0.49.0 定稿）

> 对应总目标第 ⑥ 项："Slint 软渲染卡点修复或明确长期自写渲染器路线"。
> 本文记录 2026-11-03 起 13 个批次（v0.37 → v0.49）的证据与最终决定。

## 结论

**长期走自写渲染器（`rust/suture-gui/src/swrast.rs` + `shell.rs`），Slint 只保留为可复现参考后端。**

- 生产路径：`cargo build --release --target x86_64-unknown-none`（默认 features，无 Slint）→ 手写光栅器。
- 参考路径：`cargo build --features slint` 仍可编译，用于复现上游卡点、以及在 SutureOS 具备
  `std`/线程/epoll 之后重新评估 Slint 的可能性。

## 证据

| 观察 | 事实 | 批次 |
|---|---|---|
| `SoftwareRenderer::render()` 不返回 | 多图元 + 渐变/阴影场景下卡在 `stage: draw`（QMP 仍连着 = 死循环，不是崩溃） | v0.37 |
| `Text` 元素 panic | `i-slint-core-1.18.1/item_tree.rs:1414` 越界 | v0.37 |
| 最小场景可画 | 底色 + 单个圆角矩形能画完（`checksum=95663a01`）→ 渲染器本体/alloc/链接没问题 | v0.37 |
| 无 SSE 目标的数学代价 | `f32` 全是 `compiler_builtins` 软浮点；圆角 AA 每像素一次 `fsqrt` 就让一个窗口耗时 ~420 ms | v0.47 |
| 自写路径已达标 | 圆角查表 + 整数内部循环后：窗口阶段 −80%，整屏重绘 ≈117 ms；指针移动 13–14 fps | v0.47 |
| 需要的能力都在自写路径上落地 | 中英文点阵排版、离屏 box-blur 真亚克力、悬停/点击/拖动/键盘、窗口管理、任务栏、开始菜单 | v0.38–v0.49 |

## 为什么不"修好 Slint"

1. Slint 的软渲染卡点在 `i-slint-core` 的填充/文字路径，不是我们能在无 `std`、无线程、无 SSE 的
   ring-0 环境里安全绕过的（需要 `alloc`、`libm`、可能的线程本地存储）。
2. 即便修好，Slint 的 `f32` 布局与阴影/模糊路径仍要在同一个软浮点约束下跑；我们已经证明
   **查表 + 整数** 是唯一能接受的性能路线，这与 Slint 的通用光栅器设计取向不同。
3. Wayland/Smithay 阶段真正需要的是"合成器 + 层缓冲 + 共享内存"，而不是某个 UI 工具包的渲染器；
   自写渲染器可以按层合成直接对接 `wl_shm`。

## 何时重新评估

- SutureOS 具备 `std` 等价能力（线程、futex、epoll/poll、`memfd` + `MAP_SHARED`）之后；
- 或者上游 `SoftwareRenderer` 修复了死循环/`Text` panic，且提供"整数光栅 + 查表抗锯齿"的开关。

在此之前，**新增 UI 能力一律写在 `swrast.rs` / `shell.rs` 里**，并遵守两条硬约束：
① 逐像素路径禁止 `f32` 运算（用查表/整数）；② 每帧成本用 `[gui-shell] wsplit`、
`[gui-shell] profile/30` 打点验证，不靠感觉。
