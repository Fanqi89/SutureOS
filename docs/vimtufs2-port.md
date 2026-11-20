# VimtuFS2（只读）移植说明 — 磁盘格式 + Python 镜像生成器 + 宿主校验

状态：**已在 `C:\sutureos_ports\vimtufs2` 完整实现并真机（QEMU）验收**，等 Lead 合入主仓。
本文件是移植清单「文件系统域」最后一个自研格式缺口（VimtuFS2）的交付说明。

## 1. 来源与许可（必读）

| 项 | 内容 |
| --- | --- |
| 格式名 | VimtuFS2（VimtuOS 的磁盘文件系统） |
| 规格 + 参考实现 | VimtuOS `kernel/vfs64.h`（文件头「磁盘布局」/ inode 表 / 块链表 / 限制 / mtime 打包）、`kernel/vfs64.cpp`（`VFS_O_*`/`VFS_I_*` 偏移、`sb_verify`、`inode_ok`、`rmap_*` 映射、`vfs64_format`） |
| 参考实现许可 | **GPL-3.0**（与 SutureOS 兼容） |
| 我们的做法 | **按规格重写**：C 驱动（`kernel/fs/vimtufs2.c/.h` + `vimtufs2_vfs.c`）、Python 生成器与两个宿主校验脚本全部自己写，**没有整段拷贝 vfs64.cpp**；每个偏移在注释里标了参考行号，便于逐项复核 |
| 只读参考目录 | `C:\Users\fanqi\Desktop\自研操作系统\VimtuOS\` 全程只读，未修改任何文件 |

建议登记到 `docs/provenance.md` / `THIRD_PARTY_LICENSES.md`：

> VimtuFS2 on-disk format — spec + reference implementation: VimtuOS (`kernel/vfs64.h`,
> `kernel/vfs64.cpp`), GPL-3.0. SutureOS' `kernel/fs/vimtufs2*.c` is an independent
> re-implementation written from that specification (no VimtuOS source copied);
> the host generator/verifier under `scripts/` were written from the same spec.

## 2. 格式摘要（实现依据，数值全部与规格一致）

块 = 扇区 = 512 B，块号相对卷起始：

```
块 0                超级块：magic "VIMTUFS2"@0、version@8、sector@12、block@16、total@20、
                    root@24、bitmap@28、bitmapn@32、inode@36、inoden@40、inosz@44、
                    data@48、datan@52、flags@56、CRC32([0,60))@60、
                    v5 特性扩展头 [64,88)（featbits@64 / jx_start@68 / jx_nblk@72 /
                    quota@76 / snap@80 / featureCRC([64,84))@84）、0xAA55@510
块 1 .. 1+bmn-1      空闲块位图（1 位/块，1 = 已用；尾部超出卷的位必须置 1）
块 bp .. bp+ibn-1    inode 表（128 B/个、4 个/块）  bmn=ceil(total/4096)、ibn=ceil(inodes/4)
块 dp .. total-1     数据区（文件内容 + 一级/二级间接块）
```

inode（128 B，小端）：`type@0 / namelen@1 / rsvd16@2 / size@4 / d0..d3@8,12,16,20 /
ind@24 / parent@28 / mtime@32 / nlink@36 / kind@38 / rsvd2@39 / name@40(31B) /
dind@71 / uid@75 / gid@77 / mode@79 / 保留区@81(v5: xattr@81、snapf@85) / CRC32([0,124))@124`。

* 「目录」= parent 等于它的那批 inode 的集合；**目录没有数据块**，名字存在 inode 里。
* 块链：fs 块 0..3 直接；4..131 一级间接（1 块 ×128）；132..16515 二级间接
  （1 块 ×128 子块 ×128）；映射能力 16516 块，单文件上限取整 **16384 块 = 8 MiB**。
* 名字上限 31 B；inode 总数上限 2048；CRC32 = 标准 zlib CRC（`crc32("123456789")==0xCBF43926`）。
* mtime 打包 `(year-2000)<<26 | month<<22 | day<<17 | hour<<12 | minute<<6 | second`。

## 3. 支持范围（如实）

* **v5**（VimtuOS 当前产出，= v4 布局 + 特性扩展头）：**要求 `feature_bits == 0`**。
  journal/quota/ACL/snapshot 四个格式级特性本驱动一个都没实现（日志卷必须先重放才能读、
  ACL 需要解释 xattr 块），所以带这些位的卷**拒绝挂载并把具体位打出来**，不半盲挂载。
* **v4**：feature-free v4 卷（inode 布局与 v5 前 81 字节完全同形），保留区 [81,124) 必须为 0。
* **v3**：兼容（同样是 128 B inode），无 uid/gid/mode 字段 → 读作 root 所有 + 默认模式
  （目录 0755、文件 0644），与参考实现对旧卷的做法一致。
* **v2**（64 B inode、无 dind、27 B 名字）**不支持**：mount 打 `reason=version` 拒绝，不假装能读。
* 符号链接（type=3）结构校验通过但 open 拒绝（`ENOTSUP`）。
* **只读**：open-for-write / write / mkdir / rmdir / unlink / rename 一律 `VFS_EPERM`
  （日志 `[vimtufs2] ... refused: vimtufs2 is a read-only driver`，只打前 4 次）；`sync` = no-op 返回 OK。

## 4. 文件清单

新增：

| 文件 | 行数 | 说明 |
| --- | --- | --- |
| `kernel/fs/vimtufs2.c` | 1334 | 只读核心：超级块/inode 校验、块映射、路径解析、目录扫描、读 |
| `kernel/fs/vimtufs2.h` | 138 | 公共接口（`vimtufs2_` 前缀）+ 格式常量 |
| `kernel/fs/vimtufs2_vfs.c` | 380 | 统一 VFS 适配器，驱动名 **`vimtufs2`**，写操作 EPERM |
| `scripts/mkfs_vimtufs2_testimg.py` | 919 | 纯标准库镜像生成器 + 自带 `--verify`（独立解析） |
| `scripts/verify_vimtufs2_read.py` | 458 | 宿主独立解析器（**不复用**生成器代码） |
| `scripts/vimtufs2_acceptance.py` | 487 | QEMU + QMP 逐字符注入验收（5 个 mode） |
| `scripts/diff_serial_logs.py` | 97 | 串口日志归一化对比（回归证据：地址/时间/计数器波动不影响结论） |

改动（很小）：

| 文件 | 改动 |
| --- | --- |
| `kernel/vfs/vfs.c` | `extern const vfs_fs_driver_t vimtufs2_driver;` + `vfs_register_driver(&vimtufs2_driver);` |
| `kernel/main/kmain.c` | 一段**只读探测**：`vimtufs2_boot_probe(1)`，有卷打 1 行几何、没卷打 1 行 no-volume；不挂载、不自检项 |
| `kernel/shell/shell.c` | `mount` 用法串补上 `vimtufs2`（1 行文案） |

**没有**新增启动自检项（自检仍 16/16）、没有改 GUI/启动呈现、没有改其它文件系统。

## 5. 镜像生成器（夹具）

`py -3 scripts/mkfs_vimtufs2_testimg.py` 默认产出 `build/vimtufs2.img`
（12 MiB = 24576 块，v5；位图 1+6、inode 7+384、数据 391+24185、inode 1536、空闲 23878 块）：

| inode | 名字 | 类型 | 大小 | 块 | 说明 |
| --- | --- | --- | --- | --- | --- |
| 0 | （根） | dir | 0 | 0 | parent=自己，nlink=2（1 个直接子目录） |
| 1 | `HELLO.TXT` | file | 21 | 1 | 直接块 |
| 2 | `BIG2.BIN` | file | 153600 | 300 | 4 直接 + 一级间接 + **二级间接 2 个子块**（128+40） |
| 3 | `SUBDIR` | dir | 0 | 0 | 无数据块 |
| 4 | `INNER.TXT` | file | 27 | 1 | parent=3（子目录里的文件） |
| 5 | `EMPTY` | file | 0 | 0 | 零长度 |
| 6 | `ABCDEFGHIJKLMNOPQRSTUVWXYZ01234` | file | 13 | 1 | **正好 31 B 的名字**（边界） |

* 数据块用 **互质步长 7919 打散分配**，指针块夹在数据块之间 —— 只会「顺着读下一块」的实现必错，
  必须真正跟随 d0..d3/ind/dind 指针表。
* `--verify` 独立重解镜像：179 项检查（CRC、几何重算、inode 字段、间接块覆盖、目录 parent 链、
  逐文件 sha256、位图 == 引用集合、无双重分配）。
* `--version 4` / `--features 0xf` 可产出 v4 变体与「特性位脏」负例（两者各 179/181 项 `--verify` 通过，
  后者是**故意**让合规只读驱动拒绝的负例）。

## 6. 验收（QEMU 真跑，逐字符 QMP 注入，~75 ms/字符）

命令：

```
py -3 scripts/mkfs_vimtufs2_testimg.py
py -3 scripts/mkfs_vimtufs2_testimg.py --version 4 --out build/vimtufs2_v4.img \
      --manifest build/vimtufs2_v4_manifest.json
py -3 scripts/mkfs_vimtufs2_testimg.py --features 0xf --out build/vimtufs2_badfeat.img \
      --manifest build/vimtufs2_badfeat_manifest.json
py -3 scripts/vimtufs2_acceptance.py --mode primary
py -3 scripts/vimtufs2_acceptance.py --mode negative
py -3 scripts/vimtufs2_acceptance.py --mode v4
py -3 scripts/vimtufs2_acceptance.py --mode nodisk
py -3 scripts/vimtufs2_acceptance.py --mode combo
py -3 scripts/diff_serial_logs.py build/serial_baseline_nodisk.log build/serial_vimtufs2_nodisk.log
```

结果（全部在同一份 ISO 上真跑）：

| mode | 内容 | 结果 |
| --- | --- | --- |
| primary | v5 卷挂载 + ls/cat/stat/df/写拒绝/名字超长/缺文件 + **3 处间接块读探针** + 宿主校验 | **26/26 PASS** |
| negative | feature_bits=0xf 的卷被拒绝并打印具体位 | **8/8 PASS** |
| v4 | v4 卷挂载与读取 + 宿主校验 | **11/11 PASS** |
| nodisk | 无第二块盘：只多 1 行 no-volume + 16/16 自检 | **6/6 PASS** |
| combo | FAT32（drive 0）+ VimtuFS2（drive 1）同机共存 | **11/11 PASS** |

合计 **62/62**。

串口关键行（primary，原样摘录）：

```
[vimtufs2] volume ready on drive 1: version=5 blocks=24576 inodes=1536 free=23878 (read-only; mount with "mount vimtufs2 1 /mnt/v2")
[vimtufs2] mount drive=1 version=5 blocks=24576 inodes=1536 inode=128B data=391+24185 free=23878
[vimtufs2] vfs mount drive=1 at /mnt/v2 (volume slot 0, read-only)
[vfs] Mounted 1 on /mnt/v2 (vimtufs2)
[vimtufs2] file HELLO.TXT inode=1 size=21 blocks=1
Hello from VimtuFS2!
(21 bytes)
[vimtufs2] file INNER.TXT inode=4 size=27 blocks=1
(27 bytes)
[vimtufs2] file EMPTY inode=5 size=0 blocks=0
(0 bytes)
  Size: 153600 bytes                     <-- stat /mnt/v2/BIG2.BIN（300 块，含二级间接）
[vimtufs2] name too long: 32 bytes (limit 31) in path "/ABCDEFGHIJKLMNOPQRSTUVWXYZ012345"
cat: /mnt/v2/ABCDEFGHIJKLMNOPQRSTUVWXYZ012345: invalid argument
[vimtufs2] open for write refused: vimtufs2 is a read-only driver
write: /mnt/v2/NOPE.TXT: operation not supported
vimtufs2        1               12288      349       11939  /mnt/v2      <-- df
```

`BIG2.BIN`（300 块）的三处 `hexdump` 读探针证明**客人驱动真的跟指针表**（不是顺着读下一块）：

```
00000800  92 b1 d0 ef 0e 2d 4c 6b 8a a9 c8 e7 06 25 44 63 |.....-Lk.....%Dc|   <- 一级间接映射的第 1 块
000109a0  f9 18 37 56 75 94 b3 d2 f1 10 2f 4e 6d 8c ab ca |..7Vu...../Nm...|   <- 二级间接映射的第 1 块
000249f0  69 88 a7 c6 e5 04 23 42 61 80 9f be dd fc 1b 3a |i.....#Ba......:|   <- 第二个 dind 子块（文件尾）
```

验收脚本用生成器的同一公式（`b[i]=(i*31+(i>>8)*7+0x5A)&0xFF`）独立算出期望字节，逐字节比对通过。

negative mode：

```
[vimtufs2] no volume on drive 1 (reason=feature feature_bits=0xf (journal|quota|acl|snapshot) unsupported)
[vimtufs2] mount FAILED drive=1 lba=0 (reason=feature feature_bits=0xf (journal|quota|acl|snapshot) unsupported)
mount: /mnt/bad: operation not supported (-8)
```

nodisk 回归（`diff_serial_logs.py`，地址/时间/计数器已归一化）：

```
[diff] baseline: 272 lines (268 stable)   current: 274 lines (270 stable)
[diff] added stable lines = 2
   + [vfs] Registered driver: vimtufs2
   + [vimtufs2] no volume on drive 1 (reason=no-device)
[diff] removed stable lines = 0
[diff] every stable baseline line still present in order: True
[diff] RESULT: PASS
```

宿主独立校验（两个镜像都跑，且同时证明**镜像逐字节未被客人改动**）：

```
[vimtufs2-verify] checks=173 ok=173 fail=0
[vimtufs2-verify] RESULT: PASS
```

镜像 sha256：`vimtufs2.img` 4a654da5416c23ba71e307f5048a7a0096027e7d47004c2717ac76d437095764，
`vimtufs2_v4.img` 78aff1c5fbd5c03feaf9cc234b34aa42ae5ef897aa234d90aaf528c5ba9f06fa，
`vimtufs2_badfeat.img` 55c6827cdddc17451509808e218ef3be4df7343c56e398aba4870ee241f919b2（各 12,582,912 B）。

## 7. 已知限制

1. **只读**：没有写路径（不需要日志重放/配额/ACL/快照，所以带特性位的卷直接拒绝）。
2. **不读 v2 卷**（64 B inode 布局）；符号链接不解析。
3. `stat` 的 `mtime` 报 0：VimtuFS2 存的是打包 32 位时间，`vfs_stat_t` 要 epoch 秒，
   驱动不伪造换算（`vimtufs2_stat()` 仍把原始打包值交出去）。
4. 列目录/查名字是 **O(inode 数) 线性扫描**（格式本身没有目录项）：按 16 扇区（64 个 inode）一次
   读盘缓解，12 MiB 卷扫全表 = 24 次 8 KiB 事务；大卷（2048 inode）会慢一些。查一个不存在的名字
   要扫全表（实测无感，但没有索引）。
5. inode 只有 1 扇区窗口缓存，没有全表缓存/块缓存（够用但不极致）。
6. 多卷：驱动 4 个卷槽、VFS 挂载点 4 个，与 ext2 一样不支持嵌套挂载点。
7. 没有做坏卷模糊测试（magic/CRC/version/几何/特性位/名字超长/未映射块这些路径都有明确拒绝 +
   日志，但没有系统性的 fuzz 夹具）。

## 8. 集成说明（Lead 合入主仓时）

1. 复制新增文件：`kernel/fs/vimtufs2.c`、`kernel/fs/vimtufs2.h`、`kernel/fs/vimtufs2_vfs.c`、
   `scripts/mkfs_vimtufs2_testimg.py`、`scripts/verify_vimtufs2_read.py`、
   `scripts/vimtufs2_acceptance.py`、`scripts/diff_serial_logs.py`、本文件。
2. `kernel/vfs/vfs.c`：`vfs_init()` 里加 `extern const vfs_fs_driver_t vimtufs2_driver;` 和
   `vfs_register_driver(&vimtufs2_driver);`（两行，别漏 extern）。
3. `kernel/main/kmain.c`：在 squashfs 探测块之后、ISO9660 块之前加
   `{ extern int vimtufs2_boot_probe(int drive); (void)vimtufs2_boot_probe(1); }`。
4. `kernel/shell/shell.c`：`mount` 用法串加 `vimtufs2`（可选，纯文案）。
5. 构建脚本**不需要改**：`scripts/build.py` 用 `kernel/**/*.c` 通配，新文件自动进内核。
6. QEMU 参数：`-drive id=bdisk,file=build/vimtufs2.img,format=raw,if=ide,index=1`（=drive 1），
   然后 `mount vimtufs2 1 /mnt/v2`。ISO 构建临时目录已用 `C:/sutureos_iso_temp_vimtufs2`。
7. 验收跑 §6 的命令；期望 62/62 + 回归 diff PASS + 自检 16/16 不变。
