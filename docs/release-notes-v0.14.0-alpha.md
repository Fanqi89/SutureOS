## 🚀 本次更新 (v0.13.0 → v0.14.0)

**FAT32 长文件名（VFAT LFN）读通了。** `ls /mnt/fat` 现在显示
`Long file name test.txt`，而不是短名别名 `LFNTRAN.TXT`。

### 📚 这次"缝合"的来源说明
- **Cinux 没有 FAT 驱动**（只有 ext2 + 一份 TODO 文档）→ 长名机制改从
  **PlantOS（ChaN FatFs `ff.c`）** 与 **SpiritFoxOS（本驱动的祖先 `fat32.c`）** 取，
  并补上祖先缺的**短名校验和验证**。
- **MOOS 的 FAT 不可移植**：那是原封 ChaN FatFs R0.13 塞在一个 **C# 内核**里，而且是死代码
  （`//new FATFS();`）。本版只借它的**算法清单**（SFN `~N`、LFN 写入、NTres、FSInfo 信任协议）。

### ✨ 新增

- **VFAT 长名读取**：LFN 条目状态机（属性精确 0x0F、ord 链 0x40 标志、13 个 UTF-16
  单元偏移、SFN 校验和、0x0000/0xFFFF 终止、0xE5/0x00 语义）
- **`ls` 显示长名**；`path_resolve` 在分量非 8.3 时回退长名扫描 →
  stat/open/cat/unlink/rmdir 都支持长名
- **unlink/rmdir 连带标记整条 LFN 链为 0xE5**（不再留下孤儿条目）
- **夹具与宿主校验器**：夹具写真实 LFN 链；校验器跳过 LFN/卷标条目
  （原来把 LFN 当文件解析，簇号 0 → 读出界崩溃）

### 🔧 修复

- 新建文件属性 **0x01 只读 → 0x20 归档**
- LFN 判定 `(attr & 0x0F)==0x0F` 会误判 0x1F/0x2F/0x4F 目录 → 改为精确 `attr==0x0F`

### ✅ 验证

- 客人：`ls /mnt/fat` → `Long file name test.txt`
- 回归：`[fat32] write self-test PASS`、`ops self-test PASS (3/3)`、自检 13/14、
  `ring3 9/9`、`userspace 9/9`
- 宿主：`verify_fat32_write.py` → **checks=17 ok=17 fail=0**

### 📦 下载与体验

- 附件 `sutureos.iso`（13.13 MiB，13,772,800 字节）
- sha256：`f11f8e468eb3d9feed6eb6db5739ee0a9001e628ed81838417832d1b41258438`
- QEMU：`qemu-system-x86_64 -cdrom sutureos.iso -m 512 -smp 2 -device e1000,netdev=n0 -netdev user,id=n0 -hda fat32.img`
- 进 shell 后 `ls /mnt/fat` 应看到长名文件；`cat /mnt/fat/LFNTRAN.TXT` 读内容

### ⚠️ 已知限制

- **LFN 写入没做**：本系统还不能创建长名文件（需要多槽目录分配器 + `~N` 短名生成器）
- 长名只做 ASCII 映射（非 ASCII 显示为 `_`）
- 清单里剩下的 **HanOS 调度器 / fork+wait4** 还未开始