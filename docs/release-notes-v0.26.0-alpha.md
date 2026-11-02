## 🚀 本次更新 (v0.25.0 → v0.26.0)

**命令行补齐五个文本工具：`wc` / `head` / `tail` / `grep` / `stat`。全部走公开 VFS，ramfs、FAT32、
ext2、squashfs 一致可用；验收里用 shell 自己写出多行文件再逐条检查输出，宿主侧解析 FAT32 镜像
逐字节复核文件内容。启动呈现与 GUI 外观不变。**

---

### 📦 新增：五个文本工具

| 命令 | 作用 | 实现要点 |
|---|---|---|
| `wc <file>` | 行数 / 单词数 / 字节数 | 单遍流式扫描，256 字节缓冲，不整文件读入内存 |
| `head <file> [n]` | 前 n 行（默认 10） | 读到第 n 个换行为止，FIFO 流式 |
| `tail <file> [n]` | 后 n 行（默认 10） | 单遍扫描记录最后 n 行的偏移，再 `vfs_seek` 回读（最多 64 行窗口） |
| `grep <pattern> <file>` | 打印命中行 + 行号，末尾给命中计数 | 内核 libc 没有 `strstr`，用自有子串匹配；512 字节行缓冲，超长行会截断并标注 |
| `stat <path>` | 类型 / 字节数 | 直接映射 `vfs_stat` |

### ✅ 验收（真实 QEMU + 宿主逐字节）

| 项目 | 结果 |
|---|---|
| 造数据 | shell `write` + 4 次 `append` 生成 5 行文件 `/mnt/fat/TXT.TXT` |
| `wc` | `5 10 53 /mnt/fat/TXT.TXT`（行/词/字节，与宿主算出的数字一致） |
| `head 2` | 只打印 `alpha one` / `beta two`（第三行没有出现） |
| `tail 2` | 只打印 `delta four` / `alpha five` |
| `grep alpha` | `1: alpha one`、`5: alpha five`、`grep: 2 match(es) in /mnt/fat/TXT.TXT` |
| `stat` | `Type: regular file` / `Size: 53 bytes` |
| **宿主** | 解析 `build/fat32_ata.img`：`TXT.TXT` 内容逐字节等于五行文本 |
| 脚本 | `scripts/textutils_acceptance.py` → **7/7 PASS** |
| 回归 | 内核自检 13/14、FAT32 宿主 21/21、无 PANIC、GUI 外观门禁 **0 像素** |

### 🧪 新增脚本

- `scripts/textutils_acceptance.py`：QMP 注入 10 条命令（写文件 + 五个工具），断言串口输出，
  并在宿主侧重新解析 FAT32 镜像校验文件内容。

### ⚠️ 已知限制（如实记录）

1. `grep` 是**字面子串**匹配（不是正则），区分大小写；没有 `-i`/`-v`/`-n` 开关（行号总是打印）。
2. 单行超过 512 字节会被截断并标注 `(...)`；`tail` 的行偏移窗口最多 64 行。
3. `head`/`tail` 的 `n` 用十进制（`shell_parse_u32`，也接受 `0x` 前缀，但语义仍是行数）。
4. 没有管道（`cmd | grep x` 不支持）——shell 的管道只是自检用的 `pipetest`。
5. 大文件是流式处理，不占内存，但 `tail` 需要完整扫两遍（读盘两次）。

### 📜 出处与许可（本版）

- 五个工具都是 SutureOS 自有实现（GPL-3.0），只用公开 VFS API，没有移植外部代码。

### 📦 下载

- 附件 `sutureos.iso`（15517696 字节）
- sha256：`f9ef527ed3c5f73eb0d469a7881872c9ea9eeee46991bb1fb4623f1d257ac763`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.26.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.26.0-alpha
