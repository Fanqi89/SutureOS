## 🚀 本次更新 (v0.19.0 → v0.20.0)

**终端第一次能显示中文：16×16 点阵（GB2312 全集 7443 字形）、按整数倍双宽排布、滚动后仍然
逐像素正确；字体是 SIL OFL-1.1，许可与出处文档随仓库走。启动呈现与 GUI 外观不变。**

---

### 📦 新增：终端 CJK 中文显示

| 项目 | 内容 |
|---|---|
| 字体 | `ZhengGeDianHei-16.ttf`（素材库 HBOS/fonts，**SIL OFL-1.1**，DotGothic16 派生）→ 构建期用 Pillow 渲染成 GB2312 全集 16×16 点阵（7443 字形，268 KB blob） |
| 落点 | 新增 `kernel/term/cjk16.c/.h`、`scripts/mkfont_cjk.py`、`docs/licenses/OFL-1.1.txt`；`kernel/term/term.c` 增加 UTF-8 解码 + 双宽排布 + 覆盖层；`flanterm_backends/fb.c/.h` 增加只读 metrics 访问器 |
| 为什么 | 终端此前对非 ASCII 一律画 cp437 替换方块，`ls` 里的中文文件名不可读 |

- **UTF-8 流式解码**：`term_write`/`term_putc` 共用一个小状态机（2/3/4 字节），ASCII 原样交给 flanterm。
- **双宽排布**：宽字符先向 flanterm 写两个空格（换行/滚动/栅格全由 flanterm 负责），再按
  光标位置把 16×16 点阵贴到那两个格子上——所以中文与英文混排、折行都正确。
- **滚动安全**：贴上去的字形记录在覆盖层里；检测到「最后一行换行 = 滚动」时把覆盖层整体上移
  并重绘。**关键顺序**：fb 后端是双缓冲，必须 `flanterm_flush()` 之后再贴像素，否则下一次
  刷新会把空格画回字形上（这是首版"实心块/滚动丢失"的根因）。
- **字体回退**：码点不在子集里（或字体缺失）时退回 flanterm 的替换方块，不会崩。

### ✅ 验收（真实 QEMU + 宿主逐像素断言）

| 项目 | 结果 |
|---|---|
| 显示 | `ls /mnt/fat` 列出 `中文 长名.txt`，屏幕上四个字形**与字体点阵逐像素相同**（宿主在 1024×768 截图里按 16×16 精确匹配）：中 (3,368)、文 (21,368)、长 (48,368)、名 (66,368) |
| 滚动 | 再敲 30 个回车触发滚动：四个字形仍**逐像素相同**，且整体上移 97... 实测 y=368 → 256（整 7 行、列不变） |
| 脚本 | `scripts/cjk_terminal_acceptance.py`（QMP + numpy 像素搜索）→ **10/10 PASS** |
| 回归 | 内核自检 13/14、FAT32 三自检 PASS、无 PANIC；ASCII 终端呈现不变（启动日志/banner/提示符逐字未动） |
| 外观门禁 | `gui_appearance_check.py shot --live-shot shot2` → 动态矩形外 **0 像素** |

### 🧪 新增脚本 / 文件

- `scripts/mkfont_cjk.py`：从 TTF 渲染 GB2312 全集点阵 → `build/font_cjk.bin`（构建时自动跑，缓存）。
- `scripts/cjk_terminal_acceptance.py`：启动、`ls`、截图、numpy 在整屏搜索字形位图（含滚动后再验）。
- `docs/licenses/OFL-1.1.txt`：SIL Open Font License 1.1 全文（字体随附要求）。
- `docs/provenance.md` / `docs/licensing-audit.md`：字体条目（OFL-1.1、name 表实读证据）。

### ⚠️ 已知限制（如实记录）

1. 只覆盖 **GB2312**（7443 字形）：GBK/GB18030 扩展字、生僻字、emoji 仍显示替换方块。
2. 字体是**位图 16×16**，不随字号缩放；GUI 里的中文（窗口标题/应用文本）还是 ASCII 字库，
   这一步只做了终端（GUI CJK 排下一批）。
3. 覆盖层容量 512 个字形（超出丢最旧 1/4），并且**只跟踪终端滚动**：`clear` 会清空覆盖层，
   非滚动方式重绘（例如 ANSI 光标定位移动文字）不会重排字形。
4. 中文输入还不支持（键盘/输入法仍只有 ASCII）。

### 📜 出处与许可（本版）

- 字体：**ZhengGeDianHei-16.ttf**（Copyright 2026 yzdnn；派生自 Fontworks DotGothic16）
  —— 字体内嵌 `name` 表实读为 **SIL OFL-1.1**；仓库内附 OFL-1.1 全文、出处与生成脚本。
- 渲染/覆盖层/解码：SutureOS 自有实现（GPL-3.0）。

### 📦 下载

- 附件 `sutureos.iso`（15321088 字节）
- sha256：`72830ee1960f17e44f9af3d46588495f9809739bc7586ce9f52ed207126a892f`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.20.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.20.0-alpha
