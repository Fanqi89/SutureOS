## 🚀 本次更新 (v0.37.0 → v0.38.0)

**新渲染器接上文字了**：中文（GB2312 16×16 点阵）和英文（8×16）都能在新风格样板上绘制，
**逐字形像素验收 18/18**（新加的 5 项文字检查：字形位图逐像素精确匹配）。文字用的是内核已有的
两套字体资产，没有给镜像再加第二份字体数据。

---

### 📦 文字渲染（v0.38.0）

| 位置 | 内容 | 验收方式 |
|---|---|---|
| 面板标题 | `SutureOS 设置`（ASCII + 中文，2 倍缩放） | 中文 `设` 字形逐像素匹配（hit=232, miss=0） |
| 副标题 | `圆角 + 轻亚克力 + 微透叠层` | `圆` 字形精确匹配（hit=83, miss=0） |
| 列表行 | `账户与登录` / `显示与外观` | `账` 字形精确匹配（hit=74, miss=0） |
| 卡片 | `个性化` / `控制面板` | `个` 字形精确匹配（hit=25, miss=0） |

实现（`rust/suture-gui/src/swrast.rs`）：

- `text()`：自写 UTF-8 解码（1–4 字节），逐字符取字形、按 `scale` 最近邻放大、按 alpha 合成；
  ASCII 前进 10×scale、中文 18×scale。
- 字形来源**复用内核字体**：`kernel/gui/slint_bridge.c` 新增 `slint_bridge_ascii()` /
  `slint_bridge_cjk()` / `slint_bridge_cjk_ready()`，分别从 `kernel/drivers/font.c` 的 `font8x16`
  与 `kernel/term/cjk16.c` 的 GB2312 blob 取位图给 Rust 侧 —— 不复制字体表（CJK blob 238 KiB）。
- Rust 侧 `extern "C"` 直接调这两个桥函数，`no_std` 无分配。

### ✅ 验收（真 QEMU framebuffer + 逐像素）

| 项目 | 结果 |
|---|---|
| 文字 | `scripts/slint_gui_acceptance.py` → **18/18 PASS**（原 13 项 + 5 项文字，含 7443 字形 blob 校验） |
| 整帧 | `[gui-slint] frame rendered 1024x768 checksum=... OK`，无 PANIC，命令正常回终端 |
| 原 13 项 | 圆角 / 半透明 / 假阴影 / 伪 3D 图标全部保持通过（迭代中色块位置未变） |
| 回归 | C 桌面 `apps_taskbar` **9/9**、GUI 外观门禁 **0 像素**、内核自检 13/14、`[rust] core ... PASS` |

截图：`docs/screenshots/slint-prototype-v0.38.0.png`。

### ⚠️ 已知限制（如实记录）

1. 字体只有 GB2312 子集 + ASCII 32..126：GBK 扩展汉字、emoji、其它语言会画成空白/跳过。
2. 每字符逐像素最近邻放大，2 倍缩放笔画会变粗（没有 SDF/抗锯齿字体）；正文 1 倍最清晰。
3. 文字**不换行、不裁剪**（超出面板会画到面板外，靠调用方算好位置）；没有排版引擎（行距/字距固定）。
4. 仍是静态样板帧：无输入/动效/窗口管理；没有真模糊（下一批亚克力）。
5. 文字 alpha=255 以便逐像素验收；真实界面应按层用不同 alpha（渲染器已支持 alpha 参数）。

### 📜 出处与许可（本版）

- 文字渲染是 SutureOS 自有实现；字体资产是既有 CJK 字体（SIL OFL-1.1，见 `docs/licenses/OFL-1.1.txt`）
  与内核 `font8x16`，本版未新增字体文件。

### 📦 下载

- 附件 `sutureos.iso`（17340416 字节）
- sha256：`4a9a2544440f59015ffcc0a2a50f039468504882a96ef3cf2be249709627bf1e`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.38.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.38.0-alpha
