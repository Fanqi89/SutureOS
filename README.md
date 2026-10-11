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

## ✨ 已实现功能清单

### Phase 1-2: 基础架构与驱动
- ✅ **多引导支持**: Multiboot1 + Limine (UEFI/BIOS 双模式)
- ✅ **内存管理**: 物理页分配 (PMM)、Slab 分配器、堆分配、页表管理 (4 级页表)
- ✅ **调度器**: EEVDF (公平调度)、优先级调度、协作式轮转、SMP 支持
- ✅ **中断/异常**: IDT、ISR、APIC、IOAPIC、HPET、PIT、串口中断
- ✅ **SMP 支持**: AP 启动 trampoline (16→32→64 位)、IPI 重调度/射击
- ✅ **驱动**: 帧缓冲/字体/PS2键盘/PIT/串口/RTL8139/RTL8168/AHCI/NVMe/VirtIO-blk

### Phase 3: 文件系统 (4 种)
- ✅ **VimtuFS2 (v4)**: 原生文件系统，支持权限(uid/gid/mode)、多卷挂载、COW
- ✅ **FatFS**: FAT32 读写、VFAT 长文件名
- ✅ **ext2/ext4**: 从 Cinux 移植，支持基本读写、目录遍历
- ✅ **SquashFS**: 只读压缩文件系统，支持 gzip/lz4/lzma/xz/zstd 多压缩算法

### Phase 4: 用户态与系统调用
- ✅ **系统调用**: 334 个 syscall (Linux x86_64 兼容编号)、syscall/sysret 快速路径
- ✅ **ELF 加载器**: PT_LOAD 段加载、用户页表构建、动态链接准备
- ✅ **EL0 切换**: syscall/sysret、swapgs、MSR_FS/GS_BASE、用户栈管理
- ✅ **内存管理**: brk/mmap/munmap/mprotect、COW fork、用户堆
- ✅ **musl libc 支持**: clock_gettime/getrandom/arch_prctl/futex/madvise/fcntl/set_tid_address/exit_group

### Phase 5: 网络与 GUI
- ✅ **TCP/IP 协议栈**: 以太网/ARP/IPv4/ICMP/UDP/TCP、拥塞控制(慢启动/拥塞避免/快速重传)、重传定时器、Unix 域套接字、SCM_RIGHTS fd 传递
- ✅ **GUI 框架**: 窗口管理(创建/销毁/移动/缩放/最小化/最大化)、脏矩形重绘、标题栏/边框/客户区、回调机制(绘制/键盘/鼠标/定时/关闭)、任务栏/开始菜单/桌面图标、中英双语、重启/关机

### Phase 6: 专业 ISO 发布
- ✅ **双引导 ISO**: Limine (UEFI + BIOS 双模式)、GRUB 预留
- ✅ **标准布局**: `/boot` `/system` `/EFI` 标准目录结构
- ✅ **内核双份**: `/boot/kernel.elf` + `/system/kernel/kernel.elf`
- ✅ **EFI 启动**: `/EFI/BOOT/BOOTX64.EFI` (Limine UEFI)
- ✅ **BIOS 启动**: `boot/limine/limine-bios-cd.bin`
- ✅ **初始化镜像**: `/system/initrd/initrd.tar.gz`
- ✅ **系统清单**: `/system/manifest.json`
- ✅ **ISO 大小**: 10.8 MB

### Phase 7: Linux 兼容层 (进行中)
- ✅ **syscall ABI**: 334 个 Linux syscall 号完整映射到 SutureOS 原生 syscall
- ✅ **musl 动态链接器**: 基础框架完成 (ld.so + dlopen/dlsym/dlclose/stubs)
- ✅ **deb 包安装器**: 基础框架完成 (ar 解析、control.tar.gz/data.tar.gz 解压、依赖 DB、版本比较、维护脚本桩)

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