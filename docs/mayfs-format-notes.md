# MayFS 格式说明（移植自 MayOS）

本文件是 SutureOS 中 `kernel/fs/mayfs.c` / `mayfs_vfs.c` 与
`scripts/mkfs_mayfs_testimg.py` / `scripts/verify_mayfs_read.py` 的唯一格式依据。
内容是**实读** `MayOS_0.3.0-pre1/kernel/fs.c`（14,483 B）与
`MayOS_0.3.0-pre1/kernel/core.h`（2,378 B，`File` / `FS_MAX` / `FILE_CAP`）
之后重写的字节级规格，不是对原实现的逐行翻译。

## 1. 来源与许可（实读结论）

| 项目 | 结论 |
| --- | --- |
| 参考实现 | `MayOS_0.3.0-pre1/kernel/fs.c`（MayFS 完整实现：双槽快照 + 校验和 + generation + ATA PIO） |
| 结构定义 | `kernel/core.h`：`typedef struct { u32 used, dir, parent, size; char name[48]; u8 data[FILE_CAP]; } File;`、`FS_MAX 48`、`FILE_CAP 4096` |
| 实读许可 | **MIT License，`Copyright (c) 2024 MayOS Project`**（`C:\Users\fanqi\Desktop\自研操作系统\MayOS\LICENSE`，21 行全文） |
| 与素材库清单的差异 | 素材库/移植清单把 MayOS 记为 "GPL-3.0 同族"；**实读是 MIT**。按任务要求以实读为准：本移植因此许可风险更低（MIT 可并入 GPL-3.0-or-later 的 SutureOS），本项目仍按"不整段拷贝"的规格自写实现，代码文件头注明「格式与算法来自 MayOS kernel/fs.c，实读许可 MIT，实现自写」。 |
| 未改动 | 参考目录 `...\自研操作系统\MayOS\` 只读，未写入任何文件 |

## 2. 它是什么（以及为什么值得移植）

MayFS 不是常规的"分配位图 + inode + 数据块"文件系统，而是**整卷快照式**：

* 整卷状态 = 一张固定大小的扁平记录表（48 条 × 4160 B）；
* 这张表 + 一个 512 B 头，构成一个 **256 KiB 快照**；
* 磁盘上有 **两个快照槽**（主槽 LBA 0、备槽 LBA 512）；
* 每次提交都**重写两个槽**，先备槽后主槽，每槽先写数据后写头，
  因此任何时刻掉电，至少有一个槽是完整可读的旧/新版本；
* 挂载时读两槽、各自校验和 + generation，选"有效且较新"的一个。

代价是每次提交写 512 KiB；收益是没有分配器、没有间接块、掉电语义极强。
对移植而言，它是一个**小而完整、可写、可验证掉电安全**的真实文件系统。

## 3. 扇区布局

扇区 = 512 字节（ATA PIO 原生扇区）。

| 区域 | LBA | 字节偏移 | 大小 | 说明 |
| --- | --- | --- | --- | --- |
| 主槽（primary） | 0..511 | 0 .. 262143 | 262144 B | 完整快照 |
| 备槽（backup） | 512..1023 | 262144 .. 524287 | 262144 B | 与主槽同格式的完整快照 |
| 合计 | 0..1023 | 0 .. 524287 | 512 KiB | MayFS 卷的最小尺寸（`FS_TOTAL_SECTORS = 1024`） |

测试镜像用 2 MiB（4096 扇区）：LBA 1024 之后全部为 0，驱动不读也不写。
主/备槽内的布局完全相同：

| 槽内偏移 | 大小 | 内容 |
| --- | --- | --- |
| `0x000` | 8 B | 魔数 `"MAYFS001"`（无 NUL） |
| `0x008` | 4 B | u32 小端：**记录表**的 FNV-1a 32 校验和（覆盖 `0x200 .. 0x30DFF`） |
| `0x00C` | 4 B | generation 标签 `"MAY2"`（没有这 4 字节 = 旧版快照，generation 记 0） |
| `0x010` | 4 B | u32 generation |
| `0x014` | 4 B | u32 `~generation`（与 `0x010` 互为反码，用于拒绝只写了一半的 generation） |
| `0x018` | 488 B | 0 |
| `0x200` | 199680 B | **记录表**：48 × 4160 B（见第 4 节），结束于 `0x30E00 = 200192` |
| `200192` | — | 表区结束（正好 = 扇区 391 的边界：主槽扇区 0..390 被头+表占用） |
| `256000` | 44 B | **prefs 记录**（`500 * 512`，见第 6 节） |
| 其余 | — | 0 |

要点：

* 记录表从槽内偏移 512 开始，长度 `48 * 4160 = 199680`，
  `512 + 199680 = 200192 = 391 * 512`，所以头+表恰好占用槽的前 391 个扇区；
* prefs 记录单独放在**第 500 个扇区**（`PREFS_OFFSET = 500 * 512 = 256000`），
  与表区不重叠，这是 MayOS 特意留的"槽尾空隙"；
* 槽内扇区 391..499、501..511 未被使用，提交时整槽由 0 填充；
* 全部多字节字段为**小端**。

## 4. 记录（File）结构与目录表

每条记录 4160 字节，与 MayOS `File` 逐字节一致（`_Static_assert` 在 `mayfs.c` 里守护）：

| 记录内偏移 | 类型 | 字段 | 语义 |
| --- | --- | --- | --- |
| `+0` | u32 | `used` | 0 = 空槽；合法记录必须**恰好为 1** |
| `+4` | u32 | `dir` | 0 = 普通文件，1 = 目录（其他值非法） |
| `+8` | u32 | `parent` | 父目录的记录号（0..47）；根记录自身为 0 |
| `+12` | u32 | `size` | 文件字节数；目录恒为 0 |
| `+16` | char[48] | `name` | UTF-8/ASCII 名字，NUL 结尾；`name[47]` 必须为 0 |
| `+64` | u8[4096] | `data` | 文件内容（目录不用） |

**没有**时间戳、权限位、uid/gid、符号链接、硬链接、稀疏空洞、扩展属性。

固定根记录规则（`fs.c` 的 `snapshot_valid` 硬编码，必须满足，否则整个卷判为无效）：

* 记录 **0..3 必须全部 used、dir=1、parent=0**；
* 记录 0 是卷根（MayOS 默认命名 `disk1`），记录 1..3 是它的前三个子目录
  （MayOS 默认 `Documents` / `Desktop` / `Trash`）；
* 也就是说每个 MayFS 卷的根目录里至少有三个目录；
* 本移植把记录 0..3 视为**格式保留记录**：不允许删除/改名（返回 `EINVAL`），
  创建新条目从记录 4 开始找空槽。

其它结构校验（全部照 `fs.c` 的语义）：

* 每条 used 记录：`used == 1`、`dir <= 1`、`parent < 48`、`size < 4096`、
  `name[47] == 0`、`data[size] == 0`（**载荷后必须紧跟一个 0 字节**）；
* 每条 used 记录的 parent 链必须逐级是"used 且 dir"的记录，
  最终终止于 0，链长 < 48（防环、防越界）。

## 5. 校验和算法与覆盖范围

`fs.c` 里的 `checksum()` 是 **FNV-1a 32**：

```
h = 2166136261
for each byte b:  h = (h XOR b) * 16777619      (mod 2^32)
```

覆盖范围：

* 头部 `0x008` 的校验和 = FNV-1a(槽内偏移 `0x200` 起 **199680 字节的整张记录表**)；
  **不覆盖头部自身**（自校验没有意义），也不覆盖 prefs；
* prefs 记录有自己的校验和：FNV-1a(prefs 32 字节) 存在 prefs 记录偏移 +8。

实现与验证侧（`scripts/verify_mayfs_read.py`）各自独立实现同一算法，
互不 import，用同一算法双向核对。

## 6. prefs 记录（设置，SutureOS 只做透传）

| prefs 内偏移 | 大小 | 内容 |
| --- | --- | --- |
| `+0` | 8 B | 魔数 `"MAYPREF1"` |
| `+8` | 4 B | u32 FNV-1a(prefs 数据 32 B) |
| `+12` | 32 B | 32 字节不透明数据 |
| `+44` | — | 0（到扇区尾） |

* 存在于槽内 `500 * 512 = 256000`，长 44 字节；
* MayOS 用它存"用户设置"，只有曾经保存过设置才有这条记录；
* 本移植**只做字节级透传**：挂载时读入（校验通过才算存在），提交时原样写回，
  因此 SutureOS 写过的 MayFS 卷拿回 MayOS 仍能读到设置；
  SutureOS 侧没有消费者，也没有暴露 VFS 接口；
* 校验失败/魔数不符 = 视为不存在（不阻止挂载，只打 `prefs=no`）。

## 7. generation 与槽选择规则

`generation` 是"快照版本号"，每提交一次 +1（u32 回绕）。头里同时存反码，
读侧只要 `*(u32*)(img+20) != ~*(u32*)(img+16)` 就把该快照的 generation 当 0。

挂载（`mayfs_mount`）对两槽各自做"魔数 + 表校验和 + 结构校验"，然后：

| 主槽 | 备槽 | 行为 |
| --- | --- | --- |
| 有效 | 有效 | 取 generation **较大**的一个；比较用无符号回绕安全的 `(int32_t)(gen_b - gen_a) > 0`（与 `fs.c` 一致）。相等则用主槽。 |
| 有效 | 无效 | 用主槽（若备槽只是空白，不告警；若备槽看起来损坏，打一行说明） |
| 无效 | 有效 | 用备槽，日志 `recovered from the backup slot ...` |
| 两个都全 0（全新盘） | — | 生成默认树（记录 0 = `disk1`，1..3 = `Documents`/`Desktop`/`Trash`），立即提交 generation 1。这是 MayOS `fs_init()` 对新盘的行为。 |
| 两个都无效但**不是**全 0 | — | **拒绝挂载**（`EIO`），日志 `disk left untouched`，一个字节都不写。绝不"自动修复/重格式化"未知磁盘。 |

`generation` 不参与校验和，但反码字段 + 表校验和一起，使"头写了一半"的快照
要么魔数为 0（直接无效），要么 generation 反码不符（按 0 处理）。

## 8. 提交顺序与掉电安全语义

`mayfs_commit()` 先在内存里构造新快照（表 + 新 generation = G+1 + prefs），
然后按固定顺序写盘。**每槽三步：先把头扇区清零（作废该槽）→ 写 511 个数据扇区 →
最后写头（发布新版本）→ ATA FLUSH CACHE**。两个槽的顺序固定为**先备槽、后主槽**。

设磁盘上当前两槽都是 G（或主槽 G、备槽 G-1 之类），提交过程的状态表：

| 步骤 | 主槽（LBA 0..511） | 备槽（LBA 512..1023） | 掉电后果 |
| --- | --- | --- | --- |
| 0. 提交前 | G 有效 | G 有效 | 正常 |
| 1. 备槽头清零 | G 有效 | 魔数被清 = 无效 | 挂载用主槽 G（丢本次未提交的修改，但数据完好） |
| 2. 备槽写数据 | G 有效 | 无效（头仍是 0） | 同上 |
| 3. 备槽写头 + flush | G 有效 | **G+1 有效** | 挂载用备槽 G+1 |
| 4. 主槽头清零 | 无效 | G+1 有效 | 挂载用备槽 G+1 |
| 5. 主槽写数据 | 无效 | G+1 有效 | 同上 |
| 6. 主槽写头 + flush | **G+1 有效** | G+1 有效 | 正常，两槽一致 |

因此：**任何时刻至少有一个槽是完整快照**；被中断的槽"先作废头"所以不会被误当成
有效快照；下一次提交会把被中断的槽整体重写修复（`--recovery` 验收里实测：
主槽损坏 + 备槽 gen 9 → 挂载选用备槽 → 一次提交后两槽都是 gen 10 且逐字节相同）。

## 9. 容量与限制（格式本身）

| 项 | 值 | 说明 |
| --- | --- | --- |
| 记录总数 `FS_MAX` | **48** | 1 个根 + 3 个固定目录 + 最多 **44** 个用户条目（文件或目录总数） |
| 名字长度 | **47 字符**（`name[48]` 含 NUL） | 不允许 `/`、不允许 `< 0x20` 控制字符，不允许 `.` / `..` |
| 单文件大小 | **4095 字节** | `FILE_CAP = 4096` 且格式要求 `data[size] == 0`，所以最大 4095；超过上限的写返回 `ENOSPC`，且因为 `vfs_write_file()` 把整个缓冲交给一次 `write()`，**不会留下半截文件** |
| 卷大小 | 最小 1024 扇区（512 KiB） | 只用前 512 KiB；测试镜像 4096 扇区（2 MiB） |
| 目录 | 扁平记录 + `parent` 链 | 层级不限（受 48 条记录与链长校验限制） |

**重要缺口（如实记录）**：任务书写的是 "`BIG.BIN`（>4 KiB）"，但 MayFS
**无法表示 > 4095 字节的文件**（`FILE_CAP` 与 `data[size] == 0` 不变式）。
测试夹具因此把 `BIG.BIN` 造成 **4095 字节 = 该格式能存的最大文件**，
并在验收里断言"最大合法文件被完整存取"。这不是实现偷懒，是格式上限；
要存 > 4 KiB 需要改格式（不在"移植"范围内，会破坏与 MayOS 的兼容）。

其它没有的东西（如实列出，不做假实现）：时间戳/mtime、权限与 uid/gid、
符号链接、硬链接、稀疏文件、追加写语义（`O_APPEND` 当普通偏移写）、
目录顺序保证、fsck/修复工具、多卷（本移植一次只挂一个 MayFS 卷）。

## 10. 在 SutureOS 里的映射关系

| 位置 | 内容 |
| --- | --- |
| `kernel/fs/mayfs.h` | 几何常量、错误码、`struct mayfs_entry`（= 磁盘记录）、句柄、API |
| `kernel/fs/mayfs.c` | 校验和、槽读/写、提交、挂载（双槽选择/恢复/新盘初始化）、路径层、文件/目录句柄、只读启动探测 |
| `kernel/fs/mayfs_vfs.c` | `vfs_fs_driver_t mayfs_driver`，驱动名 **`"mayfs"`**，形状同 `ext2_vfs.c` |
| `kernel/vfs/vfs.c` | `extern` + `vfs_register_driver(&mayfs_driver)`（各 1 行） |
| `kernel/main/kmain.c` | 启动时一次**只读**探测 `mayfs_boot_probe(1)`：有卷打 1 行 geometry，无卷打 1 行 no-volume；不挂载、不自检、不写盘 |
| shell | `mount mayfs 1 /mnt/may`、`umount /mnt/may`、`ls`/`cat`/`stat`/`write`/`sync`（通用 VFS 命令，无需新命令） |

**设备字符串**：`"1"`（块驱动号，1 = 第二块 IDE 盘 = QEMU `-drive if=ide,index=1`）、
`"sdb"`、`"/dev/sdb"` 三种写法等价。`"0"` = 第一块 IDE 盘。

**写语义**：`open(O_CREAT|O_TRUNC|O_WRONLY)` + `write` 只改内存记录表；
`sync`（VFS 逐挂载点调用 driver->sync）或 `umount` 才触发 `mayfs_commit()`
（与 MayOS `fs_sync()` 的显式提交语义一致）。没 sync 就断电 = 丢未提交修改，
但磁盘上必定仍是"提交过的某个完整 generation"。

**`df` 口径**：MayFS 容量按"记录槽"计。`mayfs_vfs_get_free_space()` 报
`free = 空闲记录数 × 4095`、`total = 48 × 4095 = 196560` 字节载荷预算，
不假装是磁盘块容量。

## 11. 与 MayOS 原实现的有意差异（都写进代码注释）

1. **提交顺序**：MayOS 选"非活动槽先写、再镜像另一槽"；本移植**固定先备槽后主槽**。
   保证等价（每一步都至少一槽有效，见第 8 节），实现更简单；提交完成后两槽逐字节相同，
   所以"活动槽"恒为主槽，只有挂载恢复时才可能报告 `slot=backup`。
2. **单卷**：`fs.c` 用全局 `File files[FS_MAX]`；本移植把它放进卷结构并只支持
   一个已挂载的 MayFS 卷（与 ext4 驱动一致），第二次 mount 返回 `EBUSY`。
3. **改名**：MayOS 的 `fs_move` 只改 parent；本实现是路径级 `rename`
   （取新路径的父目录 + 新名字，查重、拒环、拒跨根保留记录改名）。
4. **删除**：MayOS `fs_remove` 递归删整棵子树；本移植 `unlink` 只删文件、
   `rmdir` 只删空目录（POSIX 语义），内部仍保留递归清理助手。
5. **固定根记录不可删**：记录 0..3 是格式不变式，删除会让卷在下次校验时整体失效，
   因此明确拒绝（`EINVAL`）。
6. **新增**：块设备抽象（`blk_read/blk_write/blk_flush`）替代直写 ATA 端口；
   只读启动探测；`O_TRUNC`；`df`；VFS 适配层。
7. **未实现**：MayOS 编译期的 `MAYOS_FS_POWERLOSS_SELFTEST` 诊断开关
   （测试专用、会被打进发行内核，故不移植）；改为在宿主脚本里构造损坏镜像做等价验收。

## 12. 测试镜像夹具（`scripts/mkfs_mayfs_testimg.py`）

2 MiB（4096 扇区），generation = 7，主备槽**逐字节相同**，prefs 记录存在：

| 记录 | 名字 | 类型 | 父 | 大小 | 内容 |
| --- | --- | --- | --- | --- | --- |
| 0 | `disk1` | 目录 | 0 | 0 | 卷根（格式固定） |
| 1 | `Documents` | 目录 | 0 | 0 | 固定根目录 |
| 2 | `Desktop` | 目录 | 0 | 0 | 固定根目录 |
| 3 | `Trash` | 目录 | 0 | 0 | 固定根目录 |
| 4 | `HELLO.TXT` | 文件 | 0 | 31 | `Hello from MayFS.\nSecond line.\n` |
| 5 | `BIG.BIN` | 文件 | 0 | **4095** | 格式允许的最大文件（`(i*37+11)&0xFF` 图案） |
| 6 | `SUBDIR` | 目录 | 0 | 0 | 二级目录 |
| 7 | `INNER.TXT` | 文件 | 6 | 25 | `inner file inside SUBDIR\n` |
| 8 | `EMPTY` | 文件 | 0 | 0 | 空文件 |
| 9 | `ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCDEFG.TXT` | 文件 | 0 | 15 | **47 字符名字上限** |
| 10 | `ONE.TXT` | 文件 | 0 | 4 | `one\n` |
| 11 | `DOC.TXT` | 文件 | 1 | 26 | `document inside Documents\n`（进固定目录） |

生成器 `--verify` 自检 47 项；宿主独立校验器 `scripts/verify_mayfs_read.py`
对同一镜像 33 项全过（含两槽 generation/校验和/表逐字节一致）。

## 13. 验收与复现

```bat
py -3 scripts/mkfs_mayfs_testimg.py                     :: 生成 + 自检（47/47）
py -3 scripts/mkfs_mayfs_testimg.py --verify            :: 只校验现有镜像
py -3 scripts/verify_mayfs_read.py build/mayfs.img      :: 宿主独立解析（33/33）

py -3 scripts/build.py && py -3 scripts/build_iso.py
py -3 scripts/mayfs_acceptance.py                       :: 真 QEMU + 真按键 + 写入验收
py -3 scripts/mayfs_acceptance.py --recovery            :: 主槽损坏恢复 + 双槽损坏拒挂载
py -3 scripts/mayfs_acceptance.py --regression          :: 无该盘启动，与基线日志逐行对比
```

QEMU 参数（验收脚本内置）：

```
-cdrom build/sutureos.iso -boot d
-drive id=adisk,file=build/fat32_ata.img,format=raw,if=ide          (drive 0)
-drive id=bdisk,file=build/mayfs.img,format=raw,if=ide,index=1      (drive 1)
-qmp tcp:127.0.0.1:45781,server,nowait -smp 2
```

实测结果（本机，2026-11-20）：

* `mkfs_mayfs_testimg.py` 自检 **47/47**，`verify_mayfs_read.py` 对干净镜像 **33/33**；
* `mayfs_acceptance.py` **27/27**：
  探测行 `[mayfs] volume ready on drive 1: generation=7 slot=main primary-gen=7
  backup-gen=7 records=48 name-max=47 file-max=4095`、
  `mount mayfs 1 /mnt/may` → `[mayfs] mount drive=1 generation=7 slot=main ...
  prefs=yes`、`ls`/`cat`/`stat` 正确、`mkdir`+`rmdir`（记录创建/回收）、
  两次 `mv` 改名往返（`mayfs_rename`）、`write /mnt/may/NEW.TXT hello mayfs` →
  `sync` → `[mayfs] committed generation 8 (backup slot first, then primary; both
  flushed)` → `umount` → 再 `mount` 读到 `generation=8` 且 `NEW.TXT` 内容不变；
  宿主对**被 QEMU 真写过的那份镜像**校验 **42/42**：NEW.TXT 逐字节
  `hello mayfs\n`、12 条旧记录逐条不变、generation 7→8 只 +1、
  两槽 generation 相同且表逐字节相同、快照内**只有 27 字节变化**
  （校验和 4 + generation 8 + 新记录 15）——这就是双槽提交不伤旧数据的直接证据；
* `--recovery` **13/13**：主槽损坏（表校验和不符）+ 备槽 gen 9 →
  探测/挂载选备槽，夹具可读，提交 gen 10 后两槽逐字节一致；
  两槽都损坏的卷被**拒绝挂载且镜像 sha256 未变**（不自动重格式化）；
* `--regression` **11/11**：无 MayFS 盘启动，与移植前基线日志（291 行）逐行对比
  （归一化地址/页数/ISO 块数/ticks/sectors 等与本次构建相关的易变值后），
  **只多两行**：`[vfs] Registered driver: mayfs` 与
  `[mayfs] no volume on drive 1 (no such block device)`，
  ext2/ext4/VimtuFS2/ISO9660/FAT32 全部日志与 16/16 自检不变，无 PANIC/ERROR。
