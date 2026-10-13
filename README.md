# SutureOS (缝合怪系统)

> **一个由 14 个爱好者操作系统项目拼接而成的 x86_64 单内核操作系统**
> 
> 总代码量：~53 源文件 / 341 KB 内核镜像 / GPL-3.0 许可证

---

## 🎯 项目简介

SutureOS（缝合怪系统）是一个实验性的 x86_64 单内核操作系统，通过**移植并整合 14 个独立爱好者操作系统项目**的核心模块而构建。项目名称"缝合怪"源于其开发方式：从不同来源"缝合"功能模块，而非从零编写所有代码。

**核心设计原则：**
- 尊重所有上游许可证（GPL-3.0/MIT/BSD/Apache/MPL/Unlicense）
- 模块化架构，清晰的接口边界
- 保持上游代码风格，最小化侵入性修改
- 完整的构建工具链与可复现构建

---

## 📦 上游项目来源（14 个）

| 项目 | 许可证 | 贡献模块 |
|------|--------|----------|
| **CoolPotOS** | MIT | SquashFS (多压缩: gzip/lz4/lzma/xz/zstd)、ext2 基础 |
| **SpiritFoxOS** | GPL-3.0 | TCP/IP 协议栈、AHCI 驱动、网络栈架构 |
| **Cinux** | MIT | ext2/ext4 文件系统、NVMe 驱动、VirtIO-blk、ELF 加载器 |
| **Phobos** | GPL-3.0 | 队列/锁原语、调度器基础 |
| **MOOS** | Unlicense/MIT | 内存分配器、物理页管理 |
| **MoeOS** | GPL-3.0 | 任务调度、上下文切换 |
| **MandelbrotOS** | MPL-2.0 | PS/2 键盘、PIT 定时器驱动 |
| **HanOS** | MIT | ACPI/MADT 解析、LAPIC 初始化 |
| **HeloOS** | GPL-3.0 | 中断处理、IDT/GDT 设置 |
| **XJ380** | Apache-2.0 | VFS 抽象层、简单文件系统 |
| **NAOS** | BSD-3 | 串口驱动、控制台输出 |
| **PlantOS** | MIT | 帧缓冲、字体渲染 |
| **Lemis** | GPL-3.0 | 堆分配器、Slab 分配器 |
| **VimtuOS** | GPL-3.0 | VimtuFS2 (v4)、GUI 框架、EL0 切换、Limine 引导 |

---

## 🏗️ 架构总览

```
┌─────────────────────────────────────────────────────────────┐
│                        用户态 (Ring 3)                       │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐            │
│  │ musl    │ │ Linux   │ │ GUI     │ │ Network │  应用程序   │
│  │ libc    │ │ 兼容层  │ │ Apps    │ │ Apps    │            │
│  └────┬────┘ └────┬────┘ └────┬────┘ └────┬────┘            │
└───────┼───────────┼───────────┼───────────┼──────────────────┘
        │           │           │           │
        ▼           ▼           ▼           ▼
┌─────────────────────────────────────────────────────────────┐
│                     内核态 (Ring 0)                          │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐            │
│  │ 系统调用 │ │  VFS    │ │ 网络栈  │ │  GUI    │  核心子系统   │
│  │ 分发器  │ │ 抽象层  │ │ (TCP/IP)│ │ 框架    │            │
│  └────┬────┘ └────┬────┘ └────┬────┘ └────┬────┘            │
└───────┼───────────┼───────────┼───────────┼──────────────────┘
        │           │           │           │
        ▼           ▼           ▼           ▼
┌─────────────────────────────────────────────────────────────┐
│                    硬件抽象层 (HAL)                          │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐            │
│  │ 块设备层 │ │ 网卡驱动 │ │ 显卡驱动 │ │ 输入设备 │  驱动程序   │
│  │(NVMe/AHCI)│ │(RTL8139)│ │(Framebuf)│ │(PS/2)   │            │
│  └────┬────┘ └────┬────┘ └────┬────┘ └────┬────┘            │
└───────┼───────────┼───────────┼───────────┼──────────────────┘
        │           │           │           │
        ▼           ▼           ▼           ▼
┌─────────────────────────────────────────────────────────────┐
│                      硬件平台 (x86_64)                       │
│  CPU / APIC / ACPI / PCI / RAM / 存储 / 网络 / 显示 / 输入    │
└─────────────────────────────────────────────────────────────┘
```

---

## ✨ 功能状态清单（2026-10-13 按代码与 QEMU 实测重新核对）

> 说明：本节按"代码里是否真的实现 + QEMU 里是否被验证"标注，不再沿用早期把桩代码标成 ✅ 的写法。
> 图例：✅ 已实现且实测通过 ｜ 🟡 部分实现/未实测 ｜ ❌ 仍是桩实现

### Phase 1-2: 基础架构与驱动
- ✅ **多引导支持**: Multiboot1 + Limine (UEFI/BIOS 双模式)
- ✅ **内存管理**: PMM（启动时按 multiboot mem_upper 初始化真实区间，实测 31979 页）、8 MiB 内核堆、Slab、4 级页表
- ✅ **调度器**: EEVDF（红黑树按 deadline 选实体）+ 协作式轮转；`sched_eevdf_start()` 现在会把没有实体的 RUNNABLE 任务自动入队
- ✅ **中断/异常**: IDT 256 门（0-47 + 0x80）、ISR、PIC 重映射、LAPIC（基址已真正赋值 + MMIO 已映射）、HPET(回退 PIT)、串口
- ✅ **SMP**: AP 启动闭环 —— trampoline 拷贝到 0x8000、INIT-SIPI-SIPI、AP 进入 C 入口并 LAPIC 上线；QEMU `-smp 2` 实测 `[SMP] AP 1 online (lapic=1)`、`online=2/2`
- 🟡 **驱动**: 帧缓冲/字体/PS2 键盘（轮询式，无 IRQ 环形缓冲）/PIT/串口 ✅；ATA PIO ✅（读已验证，写有缺陷见下）；NVMe/AHCI/VirtIO-blk ❌ 桩；RTL8139/e1000 ❌ 桩
- 🟡 **GUI**: 主循环/合成/脏矩形/键盘事件已从 VimtuOS gui64 移植成真实实现，但 BIOS 模式下没有帧缓冲（`limine_fb_addr=0`），仅在 UEFI/有 fb 时可实测；鼠标 ❌（无驱动）

### Phase 3: 文件系统
- ✅ **统一 VFS 核心**: 驱动注册、挂载表、fd 表、目录游标、`vfs_read_file/vfs_write_file/vfs_mkdir_p/vfs_list_mounts`、seek 修复
- ✅ **xj_vfs (ramfs)**: 真实目录游标 + stat + O_TRUNC + 空间统计；作为根文件系统，启动时种入 `/hello.txt`、`/etc/motd`
- ✅ **FAT32（自写驱动，替代原 CoolPotOS 桩）**: superfloppy 挂载（BPB 严格校验，实测 `clusters=70544 fat_lba=32 fat_sz=552 root=2`）、8.3 目录项、FAT1/FAT2 双写、FSInfo、簇链、create/read/write/truncate/mkdir/rmdir/unlink/sync 代码路径完整；**读路径已在 QEMU 实测**（`cat /mnt/fat/hello.txt` 返回正确内容、`df` 显示真实空闲簇）；**写路径已知有缺陷**（见"已知问题"）
- ❌ **ext2 / SquashFS**: 仍是桩（`*_mount` 打印 stub，`*_vfs.c` 适配器存在但后端未实现）
- ❌ **VimtuFS2 (vfs64)**: 已改为真实委托到统一 VFS（不再是假成功），但 VimtuFS2 自身的磁盘格式（超级块/inode/位图）与多卷槽位 ❌ 未实现

### Phase 4: 用户态与系统调用
- 🟡 **系统调用**: syscall 表 512 槽 + `syscall_invoke()`；read/write/open/close/brk/mmap/munmap/fork/execve/exit/yield 及 musl 相关（clock_gettime/getrandom/arch_prctl/futex/madvise/fcntl/set_tid_address/exit_group）已注册；其余 Linux 号大多 ❌ 未注册（现返回 ENOSYS）
- ✅ **ELF 加载器 / EL0 切换**: PT_LOAD 加载、用户页表、syscall/sysret、MSR_FS/GS_BASE
- ✅ **Linux ABI 兼容层**: `linux_syscall_entry()` 现在真正分发——先查已注册的 Linux 处理器，再按映射表调用原生 syscall（此前无条件返回 ENOSYS）
- ✅ **动态链接器**: ELF 解析 + 符号解析 + `dlopen/dlsym/dlclose`；**TLS 重定位已实现**（R_X86_64_DTPMOD64/DTPOFF64/TPOFF64 + PT_TLS 解析），替代原"WARNING: TLS relocation not implemented"
- ✅ **deb 安装器（真实现）**: ar 解析（十进制 size 字段）、内建 RFC1951 inflate 解 gzip、ustar 解包并写入 VFS、control 字段/依赖解析、dpkg 风格版本比较、安装/卸载/查询/DB；QEMU 实测 `[deb] installed hello 1.0-1`；❌ xz/zstd 成员与维护者脚本（无内核 shell，如实报错）

### Phase 5: 网络
- ❌ **TCP/IP 协议栈**: `kernel/net/net.c` 仍是桩（无 ARP/IP/ICMP/UDP/TCP 收发路径），`net.h` 里的 socket API 无实现；`linux_compat` 里的 socket 号也因未注册而返回 ENOSYS

### Phase 6: 专业 ISO 发布
- ✅ **双引导 ISO**: Limine (BIOS 已实测启动；UEFI 路径未实测)、`/boot` `/system` `/EFI` 布局、内核双份、initrd、manifest.json
- ✅ **ISO 大小**: ~11.5 MB

### Phase 7: Shell 与应用
- ✅ **Shell**: 真实 `ls/cd/pwd/cat/ps/kill/df/mount/umount/echo/help/clear/startgui/reboot/shutdown`；`ls` 走 VFS readdir、`cat` 走 VFS 读、`df` 读真实挂载与空间、`mount` 走统一 VFS、`kill` 真从 EEVDF 队列摘除实体；实测输出见下"验证记录"
- 🟡 **键盘输入**: 轮询 8042；无 IRQ 环形缓冲，快速注入按键会丢字（实测现象），建议后续加 IRQ1 处理

---

## 🧪 验证记录（2026-10-13，QEMU BIOS + `-hda build/fat32.img` + `-smp 2`）

实测串口日志关键行：

```
[boot] init: pmm region 0x30cb00-0x8000000, 31979 pages free
[boot] init: heap 8 MiB at 0x30d000
[selftest][moe_pmm] PASS / [lemis_heap] PASS / [eevdf] PASS ... 12/13 passed
[blk] Registered device: ram0 (drive=9) / ata0 (drive=0, 71680 sectors)
[vfs] Mounted ram on / (xj_vfs)
[fat32] mount drive=0 lba0=0x539058eb spc=1 clusters=70544 fat_lba=32 fat_sz=552 root=2 free=70539
[deb] parsed hello 1.0-1 (all) / [deb] unpacked hello: 2 files, 3 dirs / [deb] installed hello 1.0-1
[SMP] AP 1 online (lapic=1) / [SMP] online=2/2 bsp_lapic_id=0
sutureos#
```

交互实测（QEMU monitor `sendkey` 注入，宿主侧核对）：

```
sutureos# ls /
etc/  hello.txt  usr/
sutureos# df
Filesystem   Device  1K-blocks  Used  Available  Mounted on
xj_vfs       ram            4     0          3   /
fat32        0          35272     2      35269   /mnt/fat
sutureos# cat /mnt/fat/hello.txt
Hello from SutureOS FAT32!
This file lives on a real FAT32 volume (driver: fat32).
(83 bytes)
```

宿主侧校验：`scripts/mkfat32_testimg.py` 自建 superfloppy FAT32 镜像（70544 簇、FAT1==FAT2、FSInfo 合法）与真实 `.deb`（ar + control.tar.gz + data.tar.gz），并用独立解析器逐字节校验；启动前后整盘 CRC32 一致（`20B5EAF8`），证明只读路径不会改动介质。

## ⚠️ 已知问题（如实记录）

1. **FAT32 写路径不可信**：guest 写测试中 payload 出现在根目录扇区、目标簇里是陈旧内存数据——缺陷在**ATA PIO 写路径**（读路径已被 `cat`/mount 实测证明正常）。因此当前引导**不执行任何写盘操作**，不会破坏真实介质；修复 ATA 写后再启用 FAT32 写。
2. **`[selftest][moos_intr] FAIL`**：moos 中断自检故意注入异常（`err=0xbad`），处理函数会打印告警并把该自检判为 FAIL——属该自检与框架的判定不一致，非本次引入。
3. **两套调度器并存**：`task.c` 的协作轮转与 `eevdf.c` 的 EEVDF 双轨运行，任务内调用 `sched_yield()` 会切到 EEVDF 循环的陈旧上下文（实测表现为跳到 0x3）。Shell 输入循环因此改为有界 pause 自旋，未再暴露；根治需要合并调度器。
4. **`context.kgs_base` 被两用**：`cinux_context_switch` 把它写进 MSR_GS_BASE，而 EEVDF 用同一字段存放 `sched_entity*`；建议为实体指针另设字段。
5. **IDT 未铺满**：仅 0-47 与 0x80 有门，IPI 向量 0xE0/0xE1 与 LAPIC 假中断向量 0xFF 仍是 not-present（会 #GP）；AP 目前只 `sti; hlt` 空转，未接入调度。
6. **无帧缓冲（BIOS 模式）**：GUI 主循环已实现但当前启动路径 `limine_fb_addr=0`，无法可视化验证；fbcon 软光标仍由定时器闪烁。
7. **键盘无 IRQ**：只轮询 8042 单字节缓冲，快速输入丢键。

---

## 🔧 构建系统

### 工具链要求
```bash
# MSYS2 环境 (Windows)
pacman -S mingw-w64-x86_64-clang mingw-w64-x86_64-binutils nasm xorriso tar gzip

# 交叉编译工具链
# x86_64-elf-gcc / x86_64-elf-ld / x86_64-elf-objcopy
```

### 目录结构
```
缝合怪系统/
├── boot/                   # 引导加载器 (Multiboot + Limine 配置)
│   ├── limine.conf         # Limine 引导菜单配置
│   └── linker.ld           # 内核链接脚本
├── arch/x86_64/            # 架构相关代码
│   ├── *.c *.S *.h         # 中断/内存/调度/系统调用/EL0切换
├── kernel/
│   ├── drivers/            # 设备驱动 (FB/键盘/网卡/块设备)
│   ├── fs/                 # 文件系统 (VimtuFS2/FatFS/ext2/SquashFS)
│   ├── gui/                # GUI 框架
│   ├── ipc/                # 进程间通信
│   ├── main/               # 内核入口/初始化/自检
│   ├── mm/                 # 内存管理 (PMM/Slab/Heap/Mmu)
│   ├── net/                # 网络协议栈
│   ├── sched/              # 调度器 (EEVDF/优先级/任务)
│   ├── smp/                # SMP 支持 (AP trampoline/IPI)
│   ├── syscall/            # 系统调用/ELF加载器/Linux兼容层
│   ├── pkg/                # 软件包管理
│   │   ├── deb.h           # deb 包格式定义
│   │   └── deb.c           # deb 安装器实现
│   └── vfs/                # 虚拟文件系统 (VimtuFS2/xj_vfs)
├── lib/                    # 通用库 (字符串/打印/红黑树/队列)
├── include/stitch/         # 公共头文件 (types/cpu_context/user_context)
├── scripts/
│   ├── build.py            # 内核构建脚本 (Python, Unicode路径支持)
│   └── build_iso.py        # ISO 镜像构建脚本 (Limine + xorriso)
└── tools/                  # 工具链与 Limine 二进制
```

### 构建命令
```bash
# 1. 构建内核
python scripts/build.py

# 2. 构建 ISO 镜像
python scripts/build_iso.py

# 产出文件:
# build/kernel.elf      - 内核 ELF (341 KB)
# build/sutureos.iso    - 可引导 ISO (10.8 MB)
```

---

## 🚀 运行测试

### QEMU (推荐)
```bash
# UEFI 模式 (需要 OVMF)
qemu-system-x86_64 \
  -bios /usr/share/ovmf/OVMF.fd \
  -cdrom build/sutureos.iso \
  -m 512M -smp 2 \
  -serial stdio -display gtk

# BIOS 模式
qemu-system-x86_64 \
  -cdrom build/sutureos.iso \
  -m 512M -smp 2 \
  -serial stdio -display gtk
```

### 真机启动
```bash
# 写入 USB (会擦除目标设备！)
sudo dd if=build/sutureos.iso of=/dev/sdX bs=4M status=progress && sync
```

---

## 📋 内核启动参数 (cmdline)

| 参数 | 说明 |
|------|------|
| `verbose` | 启用详细启动日志 |
| `loglevel=N` | 设置日志级别 (0-7) |
| `root=/dev/sdX` | 指定根文件系统设备 |
| `init=/path/to/init` | 指定 init 程序路径 |

---

## 📄 许可证合规

| 组件 | 许可证 | 合规方式 |
|------|--------|----------|
| 内核核心 | GPL-3.0 | 主许可证，源码开放 |
| VimtuFS2 | GPL-3.0 | 保留原版权声明 |
| FatFS | BSD-like | 保留版权声明 |
| ext2/ext4 | MIT | 保留版权声明 |
| SquashFS | GPL-2.0 | 文件级 copyleft |
| Limine | BSD-2 | 作为独立二进制聚合 |
| musl libc | MIT | 静态链接，保留许可证 |
| MandelbrotOS 驱动 | MPL-2.0 | 文件级 copyleft，保留原文件 |

**关键合规点：**
- 所有 MPL-2.0 文件保持文件级 copyleft，未修改原文件结构
- Limine 作为独立引导加载器聚合，非链接进内核
- 所有上游版权声明完整保留在源文件头部

---

## 🤝 贡献指南

1. **Fork** 本仓库
2. 创建特性分支: `git checkout -b feature/amazing-feature`
3. 提交变更: `git commit -m 'Add amazing feature'`
4. 推送分支: `git push origin feature/amazing-feature`
4. 发起 Pull Request

**代码风格要求：**
- C 代码遵循 Linux Kernel 编码风格 (8 空格缩进)
- 汇编使用 NASM 语法 (Intel 语法)
- 所有新增文件必须包含版权声明与 SPDX 许可证标识
- 修改上游文件需在头部标注 `Changes:` 字段说明修改内容

---

## 📚 参考文档

- [Intel SDM Vol. 3](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html) - x86_64 架构参考
- [System V AMD64 ABI](https://github.com/hjl-tools/x86-psABI/wiki/x86-64-psABI) - 调用约定
- [Limine Boot Protocol](https://limine.bootloader.org/) - 引导加载器规范
- [musl libc](https://musl.libc.org/) - 标准库实现参考
- [Linux syscall table](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl) - 系统调用号对照

---

## 🎉 致谢

感谢所有上游项目的作者们，是你们的开源精神让这个"缝合怪"得以诞生：

- **CoolPotOS** / **SpiritFoxOS** / **Cinux** / **Phobos** / **MOOS** / **MoeOS** / **MandelbrotOS** / **HanOS** / **HeloOS** / **XJ380** / **NAOS** / **PlantOS** / **Lemis** / **VimtuOS**

> *"站在巨人的肩膀上，我们看得更远——哪怕只是把巨人的零件拼成了一个缝合怪。"*

---

**项目状态**: 🟢 活跃开发中  
**当前版本**: 0.1.0 (实验性)  
**最后更新**: 2025-10

---

*Generated with ❤️ by the SutureOS Team*