## 🚀 本次更新 (v0.24.0 → v0.25.0)

**第五个桌面应用：文本编辑器（B10）。从开始菜单打开、用真实键盘输入、回车落盘，
宿主侧解析 FAT32 镜像确认文件内容逐字节等于敲进去的文本。启动呈现与默认桌面外观不变。**

---

### 📦 新增：Text Editor（`APP_ID_EDT`，懒创建）

| 项目 | 内容 |
|---|---|
| 入口 | 开始菜单新增 **Text Editor**（与 Image Viewer 一样按需创建，默认桌面仍只有 3 个窗口） |
| 能力 | 加载 `/mnt/fat/EDIT.TXT`（存在则读入）、可打印字符插入、Backspace 删除、Enter 换行并保存、光标显示、底部状态行（路径 / 字节数 / 脏标记） |
| 实现 | `kernel/gui/apps.c` 里 SutureOS 自绘（只用 `gui_fill_rect` / `gui_draw_str`），1 KiB 缓冲，保存走 VFS `vfs_write_file` |
| 键盘 | 复用窗口管理器的 `on_key` 通道；**修正了一个老问题**：`q` / `r` 原来是无条件全局快捷键（q 退出桌面、r 重绘），在文本窗口获得焦点时会把正文里的 q/r 吃掉——现在只在没有文本输入窗口时生效 |

### ✅ 验收（真实 QEMU + 真实鼠标/键盘 + 宿主逐字节）

| 项目 | 结果 |
|---|---|
| 打开 | 真实鼠标点开始菜单 → **Text Editor** → `[gui] Window created: Text Editor (app_id=7)` |
| 输入 | QMP 注入真实 PS/2 按键（走内核键盘解码 → GUI 键盘通道 → 编辑器） |
| 保存 | 回车触发保存 → `[edit] saved 17 bytes to /mnt/fat/EDIT.TXT` |
| **宿主** | 解析 `build/fat32_ata.img`：`EDIT.TXT` 内容 = `hello editor 42\n`，**逐字节相同** |
| 脚本 | `scripts/editor_acceptance.py` → **5/5 PASS** |
| 回归 | 内核自检 13/14、FAT32 宿主 21/21、图片查看器 + GUI 中文 4/4、无 PANIC、默认桌面外观门禁 **0 像素** |

### 🧪 新增脚本

- `scripts/editor_acceptance.py`：起 GUI、点菜单开编辑器、点进窗口取焦点、注入按键、回车保存、
  截屏，并**在宿主侧解析 FAT32 镜像逐字节比对**。

### ⚠️ 已知限制（如实记录）

1. **回车即保存**：本机 PS/2 解码器不暴露 CTRL 修饰键，`Ctrl+S` 收不到 0x13，所以这一版用
   "Enter = 换行并保存"；解码器支持 CTRL 后可以把 Ctrl+S 接回来（`ed_key` 已有 0x13 分支）。
2. 只编辑一个固定文件 `/mnt/fat/EDIT.TXT`，没有打开/另存为对话框、没有文件名输入。
3. 1 KiB 缓冲、超过就静默不插入（不报错，也不会写坏文件）；单字节字符，没有 CJK 输入法。
4. 光标只能随输入移动，**没有上下左右方向键编辑**（方向键在 GUI 里被窗口移动占用了）。
5. 没有撤销/重做、没有选中/复制粘贴。

### 📜 出处与许可（本版）

- 编辑器是 SutureOS 自有实现（GPL-3.0），只用公开的 `gui.h` 绘图 API 与 VFS，没有移植外部代码。

### 📦 下载

- 附件 `sutureos.iso`（15476736 字节）
- sha256：`457b9552e01f7d14a6789998d1b613c8452646265b2f2b6ca8f091996d632234`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.25.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.25.0-alpha
