## 🚀 本次更新 (v0.20.0 → v0.21.0)

**中文从终端走进了窗口：GUI 文本绘制现在认识 UTF-8 与 16×16 点阵，图片查看器的状态/缩放行
显示中文，宿主在截图上按 16×16 逐像素断言。启动呈现与默认桌面外观不变。**

---

### 📦 新增：GUI 文本层支持 CJK

| 项目 | 内容 |
|---|---|
| 落点 | `kernel/gui/gui.c` 的 `gui_draw_text()`：UTF-8 解码 → ASCII 走原有 8×16 字库，宽字符走 `kernel/term/cjk16.c` 的 16×16 点阵，占两格（16 px） |
| 为什么 | v0.20.0 只让终端认中文；GUI 里所有文本（窗口标题、应用文本、菜单）仍只认 ASCII，中文会显示 `?` |
| 字体 | 复用 v0.20.0 的 GB2312 点阵子集（SIL OFL-1.1，7443 字形），不再额外打包字体 |

- 绘制走既有 `gui_px()`（会经过裁剪矩形），所以中文不会画出窗口边界。
- 码点不在子集/非宽字符 → 退回 `?`，不会崩。
- 默认桌面（Task Manager / Calculator / About）**文字与布局一字未改**，外观门禁仍 0 像素。

### ✅ 验收（真实 QEMU + 宿主逐像素断言）

| 项目 | 结果 |
|---|---|
| GUI 中文 | 真实鼠标点开始菜单 → Image Viewer（加载夹具图）→ 窗口底部显示 `缩放 x8`：宿主在截图里按 16×16 点阵 **精确匹配**到 缩(161,402)、放(177,402)（页脚配色 0x707880） |
| 脚本 | `scripts/gui_cjk_acceptance.py` → **4/4 PASS**（QMP 点菜单 + 像素匹配） |
| 终端回归 | `scripts/cjk_terminal_acceptance.py` → **10/10 PASS**（`ls` 中文名 + 滚动后仍精确） |
| 回归 | 内核自检 13/14、FAT32 写/ops/长名三自检 PASS、无 PANIC |
| 外观门禁 | `gui_appearance_check.py shot --live-shot shot2` → 动态矩形外 **0 像素**（默认桌面未变） |

### 🧪 新增脚本

- `scripts/gui_cjk_acceptance.py`：启动 GUI、真实点击打开图片查看器、截图、numpy 在整屏按颜色+16×16
  点阵搜索中文字形（不信任客人输出）。

### ⚠️ 已知限制（如实记录）

1. 只有 **新建窗口里的文本**用到了中文（当前是图片查看器的 `缩放 xN`）；窗口标题、开始菜单、
   Task Manager / Calculator / About 仍是英文——这些属于**冻结外观**，本批刻意不动它们。
2. 字体仍是 GB2312 子集（7443 字形）、位图 16×16 不缩放；GBK 扩展/生僻字/emoji → `?`。
3. 没有中文输入法（键盘只有 ASCII），中文只能来自文件名/程序内建字符串。
4. GUI 文本没有换行/裁剪计算（沿用原来的“逐字符画、越界由裁剪矩形挡住”语义），长中文串会被
   裁剪而不是折行。

### 📜 出处与许可（本版）

- 字体与点阵：`ZhengGeDianHei-16.ttf`（SIL OFL-1.1，Copyright 2026 yzdnn / Fontworks DotGothic16），
  许可全文在 `docs/licenses/OFL-1.1.txt`；GUI 复用同一份点阵，没有新增第三方代码。

### 📦 下载

- 附件 `sutureos.iso`（15325184 字节）
- sha256：`755e73fe1b2ad5318168b7a7d93e472fe8fc3af0c9a4dd611493046bed2073b7`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.21.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.21.0-alpha
