## 🚀 本次更新 (v0.18.0 → v0.19.0)

**终端第一次能自己写文件：`write` / `append` / `hexdump` 三条命令，只用公开 VFS API，
所以 ramfs、FAT32、ext2 都能用；写入内容由宿主侧独立解析镜像逐字节核对。启动呈现与 GUI 外观不变。**

---

### 📦 新增 1：`write <file> <text...>`

- 创建或整体替换一个文本文件（自动补行尾 `\n`），回显写入字节数。
- 以前要测任何文件系统行为，都必须先造镜像；现在终端里就能造数据。

### 📦 新增 2：`append <file> <text...>`

- 追加一行：读出现有内容（最多 2 KiB）→ 补一个 `\n`（如果原来没有）→ 重新写回。
- 文件不存在时等价于 `write`，并如实打印新的总字节数。

### 📦 新增 3：`hexdump <file> [offset] [length]`

- 经典十六进制 + ASCII 视图：`偏移  16 个 hex 字节  |可打印字符|`
- 支持 `offset`/`length`（十进制或 `0x` 十六进制），默认 128 字节；可用于检查 FAT 目录项、
  ext2 超级块、二进制夹具（含 `\0` 的内容也能看）。

### ✅ 验收（真实 QEMU + 宿主独立校验）

| 项目 | 结果 |
|---|---|
| guest 侧 | `write ... hello shell text tools` → `(23 bytes)`；`append ... second line` → `now 36 bytes`；`cat` 两行都在；`hexdump /mnt/fat/NOTE.TXT 0 32` 打出偏移列 + hex + `|hello shell text tools...|` + `(32 bytes at offset 0)` |
| 宿主侧 | 不信任客人输出，独立解析 `build/fat32_ata.img` 的 FAT32 结构：`NOTE.TXT` 存在，内容与 `"hello shell text tools\nsecond line\n"` **逐字节相同**；`HELLO.TXT`/`README.TXT` 等原有文件仍在 |
| 脚本 | `scripts/shell_text_tools_acceptance.py`（QMP 注入命令 + 8 条断言 + 宿主解析）→ **8/8 PASS** |
| 回归 | 内核自检 13/14、FAT32 写/ops/长名三个自检 PASS、无 PANIC |

### ⚠️ 已知限制（如实记录）

1. `write`/`append` 是**整文件重写**（SutureOS 的 VFS 还没有“追加”标志）：`append` 会把文件读到
   2 KiB 静态缓冲里再整体写回，所以**超过 2 KiB 的文件不能 append**（会明确报错，不静默截断）。
2. `write` 单行文本上限 512 字节；参数用空格连接，不保留多个连续空格；不支持引号/转义。
3. `hexdump` 默认读 128 字节，`length` 无上限但按 16 字节一块输出。
4. **中文（CJK）显示仍未进版**：本轮尝试了终端 16×16 点阵（字体子集 + flanterm 宽字符钩子），
   实机渲染出的是实心块，说明宽字符路径有 bug；为了不把半成品发出去，这次**回退了全部 CJK 改动**
   （`git status` 干净），下一轮重新做（已有可用的字体生成脚本思路与 HZK16/16 点阵来源评估）。

### 📜 出处与许可（本版）

- 三条命令都是 SutureOS 自有实现（GPL-3.0），只用公开 VFS API，没有移植外部代码。

### 📦 下载

- 附件 `sutureos.iso`（14764032 字节）
- sha256：`80daf379fb565e0d60d2ce6a7c14f955cbfae74f23f923c2a3e4f2e0a2cb9607`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.19.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.19.0-alpha
