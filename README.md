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
- ✅ **中断/异常**: IDT 256 门（0-47 + 0x80 + 0xE0/0xE1/0xFF）、ISR、PIC 重映射、LAPIC（基址已真正赋值 + MMIO 已映射）、**IOAPIC 真正初始化**（ABAR 式 2 MiB 恒等映射 + MADT 取址，并把 8259 级联按 ExtINT 路由到 LINT0）、HPET(回退 PIT)、串口；系统节拍仍由每核 LAPIC 定时器提供
- ✅ **SMP**: AP 启动闭环 —— trampoline 拷贝到 0x8000、INIT-SIPI-SIPI、AP 进入 C 入口并 LAPIC 上线；QEMU `-smp 2` 实测 `[SMP] AP 1 online (lapic=1)`、`online=2/2`
- ✅ **驱动**: 帧缓冲/字体/PS2 键盘（轮询式，无 IRQ 环形缓冲）/PIT/串口 ✅；**ATA PIO ✅**、**AHCI/SATA ✅**（`kernel/drivers/ahci.c`，QEMU `-device ahci` + `ide-hd` 实测 71680 扇区、注册 sata0=drive 4、FAT32 写自检 + 宿主验收 10/10）、**NVMe ✅**（`nvme.c`，admin+IO 队列、PRP、Identify，注册 nvme0=drive 8，同样 10/10）、**VirtIO-blk ✅**（`virtio_blk.c`，legacy 0.9.5 传输 + split virtqueue，注册 vda=drive 10，同样 10/10）、**RTL8139 ✅**（见 Phase 5）；e1000 ❌ 未实现
- ✅ **显示与终端**: 开机向固件请求 1024x768x32 线性帧缓冲（multiboot1 头 flag bit 2 + 运行时 `mmu_map_mmio_identity` 把 1 GiB 以外的 LFB 映射进来，实测 `[fb] ready: 1024x768 32 bpp pitch=4096 at 0xfd000000`）；屏幕默认是**黑底白字的终端**（见下节），GUI 需在终端里输入 `startgui` 才进入

### Phase 3: 文件系统
- ✅ **统一 VFS 核心**: 驱动注册、挂载表、fd 表、目录游标、`vfs_read_file/vfs_write_file/vfs_mkdir_p/vfs_list_mounts`、seek 修复
- ✅ **xj_vfs (ramfs)**: 真实目录游标 + stat + O_TRUNC + 空间统计；作为根文件系统，启动时种入 `/hello.txt`、`/etc/motd`
- ✅ **FAT32（自写驱动，替代原 CoolPotOS 桩）**: superfloppy 挂载（BPB 严格校验，实测 `clusters=70544 fat_lba=32 fat_sz=552 root=2`）、8.3 目录项、FAT1/FAT2 双写、FSInfo 同步、簇链、create/read/write/truncate/mkdir/rmdir/unlink/sync；**读路径实测**（`cat /mnt/fat/hello.txt` 正确、`df` 显示真实空闲簇）；**写路径实测通过**（启动自检 `[fat32] write self-test PASS` + `[fat32] ops self-test PASS (mkdir/rmdir, rename, unlink: 3/3)`，并由宿主侧独立解析镜像验收 `checks=17 ok=17`：新文件逐字节正确、旧文件未改动、FAT1==FAT2、FSInfo 计数与实测一致、整盘 CRC 变化、**rename 后内容逐字节保留且旧名消失、unlink/rmdir 后条目消失且 FAT 里没有孤儿簇链**）。**rename 已实现**（同一目录内的 8.3 名字段就地改写，簇链/时间戳/属性不动；跨目录移动如实拒绝并打日志，因为 8.3-only 需要新建目录项）。
- ✅ **ext2（只读，真实驱动）**: `kernel/fs/ext2.c` 解析 superblock/BGDT、单/双/三级间接块、稀疏空洞、readdir/stat/read，写操作返回 EROFS；`ext2_vfs.c` 接入统一 VFS，kmain 自动挂 `-hdb`（drive 1）并自检
- ✅ **SquashFS 4.0（只读，真实驱动）**: `kernel/fs/squashfs.c`(1895 行)+`squashfs.h`+`squashfs_vfs.c`。gzip 数据块（复用新抽出的 `kernel/pkg/inflate.c`，全树只留一份 RFC1951 inflate，`deb.c` 改为调用它）、元数据块（压缩与未压缩两种）、fragment 表、id 表、目录表、fast/extended 符号链接、多块大文件与 partial read。**宿主侧三层验证**：`mkfs_squashfs_testimg.py` 自写镜像并独立重解析 PASS；`verify_squashfs_read.py build/squashfs.img` **checks=83 ok=83**；`tests/squashfs_harness/harness.c` 用**真实驱动源码**编到宿主跑 **checks=138 ok=138**（23 组、含 11 组负例：坏 magic/版本/xz/zstd 压缩号/块大小不一致/镜像截断/IO 失败/越界 inode/越界 fragment 索引/越界数据块表/未挂载句柄）。guest 侧 `-hdb build/squashfs.img` 实测 `[squashfs] selftest PASS (hello=0x7370589f large=0x37fe697d nested=0xb5d4a9fe uncomp=0x1fe9652d frag=0x681964c1)`，并用 `verify_squashfs_read.py --guest-log` 把 guest 打印的 CRC 与镜像独立比对 **checks=95 ok=95**。与 ext2 可共存（SquashFS 也会探测 virtio 盘：`-hdb ext2.img` + virtio 挂 SqFS 实测两者同时 selftest PASS）。**不支持**：lzo/lzma/xz/lz4/zstd（mount 时明确拒绝）、xattr、export/NFS 表、目录索引项（线性 readdir）、>4096 数据块的文件、uid/gid 只回报表索引、无任何写路径（EROFS→VFS_EPERM）
- ❌ **VimtuFS2 (vfs64)**: 已改为真实委托到统一 VFS（不再是假成功），但 VimtuFS2 自身的磁盘格式（超级块/inode/位图）与多卷槽位 ❌ 未实现

### Phase 4: 用户态与系统调用
- 🟡 **系统调用**: syscall 表 512 槽 + `syscall_invoke()`；read/write/open/close/brk/mmap/munmap/fork/execve/exit/yield 及 musl 相关（clock_gettime/getrandom/arch_prctl/futex/madvise/fcntl/set_tid_address/exit_group）已注册；其余 Linux 号大多 ❌ 未注册（现返回 ENOSYS）
- ✅ **ELF 加载器 / EL0 切换**: PT_LOAD 加载、用户页表、syscall/sysret、MSR_FS/GS_BASE
- ✅ **Linux ABI 兼容层**: `linux_syscall_entry()` 现在真正分发——先查已注册的 Linux 处理器，再按映射表调用原生 syscall（此前无条件返回 ENOSYS）
- ✅ **动态链接器**: ELF 解析 + 符号解析 + `dlopen/dlsym/dlclose`；**TLS 重定位已实现**（R_X86_64_DTPMOD64/DTPOFF64/TPOFF64 + PT_TLS 解析），替代原"WARNING: TLS relocation not implemented"
- ✅ **deb 安装器（真实现）**: ar 解析（十进制 size 字段）、内建 RFC1951 inflate 解 gzip、ustar 解包并写入 VFS、control 字段/依赖解析、dpkg 风格版本比较、安装/卸载/查询/DB；QEMU 实测 `[deb] installed hello 1.0-1`；❌ xz/zstd 成员与维护者脚本（无内核 shell，如实报错）

### Phase 5: 网络
- ✅ **RTL8139 驱动 + ARP/IPv4/ICMP/UDP 协议栈（轮询模式，真实收发）**: `kernel/drivers/rtl8139.c` + `kernel/net/net.c`。链路层 DLT 收发、ARP 请求/应答与缓存、IPv4 头校验和、ICMP echo（`ping` 命令与开机自检）、UDP（含伪头校验和），shell 有 `ping <ipv4>` 与 `netstat`。
  实测（`-netdev user -device rtl8139`，宿主侧 `scripts/net_probe.py` **22 项断言全过**）：
  ```
  [net] rtl8139 found at 0xc000, irq=11, mac=52:54:00:12:34:56
  [net] IPv4 10.0.2.15/24 gw 10.0.2.2 (mode=poll)
  [net] selftest ping 10.0.2.2 -> reply in 1288 us
  sutureos# ping 10.0.2.2
    64 bytes from 10.0.2.2: icmp_seq=1 time=82 us   (3/3 应答)
  ```
  修复记录见"已知问题"第 11 条（TX 描述符门铃、ChipCmd/CBR 不可信、RX 条目步长、ICMP id/seq 字节序共 4 个真实 bug）。
- ✅ **TCP（RFC 793 子集：主动 + 被动打开、双向数据、重传、关闭）**: `kernel/net/tcp.c` + `tcp.h`。**主动打开**（`tcp_connect`，SYN + 超时重传）与**被动打开**（`tcp_listen`/`tcp_listen_accept`：SYN→SYN+ACK→ACK，子连接进 accept 队列；SYN+ACK 发不出去时保留 TCB 交给重传定时器，而不是丢掉连接）；序号/确认号记账、按 MSS 分片发送（单段停等 + 500 ms×4 重传）、双向数据、**一个乱序段缓冲槽**（先到的未来段暂存并回重复 ACK，缺口补上后按序补投）、主动/被动关闭与 TIME_WAIT、RST 收发、伪头校验和。
  三条独立验收链路（都能在 guest 串口或宿主侧判定）：
  ```
  # 1) guest 当客户端：宿主跑真实 TCP 服务器，slirp 把 guest→10.0.2.2 转到宿主 loopback
  scripts/tcp_acceptance.py        → echo 7/7（双向逐字节）、bulk 6/6（3000 字节 = 3 个 MSS 段）
  # 2) guest 当服务端：QEMU -netdev user,hostfwd=tcp::5700-:40404，宿主主动连进来
  scripts/tcp_server_acceptance.py → 8/8
     [guest] TCP listening on port 40404 / TCP accepted from 10.0.2.2:56125
             TCP serve rx 18 bytes hex=48454c4c4f2d46524f4d2d484f53542d3432 ("HELLO-FROM-HOST-42")
             TCP serve done: echoed 18 bytes, close OK
  # 3) 内核内自检：手工注入段（对端不答 ARP，纯状态机，不上网线）
  shell `tcpooo`                   → PASS
     tcpooo reassembled 30 bytes ascii="ABCDEFGHIJabcdefghijKLMNOPQRST"
     tcpooo: passive open + out-of-order reassembly PASS
  ```
  Shell 有 `tcpecho <ip> <port> <text>`、`tcpsend <ip> <port> [text]`（缺省 3000 字节）、`tcpserve <port>`、`tcpapi <port>`、`tcpooo`、`tcpstat`。
- ✅ **套接字 ABI（SOCK_STREAM 已真正接到 TCP）**: `kernel/net/net.c` 的 `socket/bind/listen/accept/connect/send/recv/shutdown/close` 现在按 `SOCK_STREAM` 路由到 `kernel/net/tcp.c`（`g_sock_tcp[]` 把 fd 映射到 TCP 连接句柄；accept 会为新连接分配一个新 fd 并回填 peer 地址）。这也是 ring-3 通过 syscall 层会走的入口。验证：`tcpapi <port>` 只用套接字 ABI 起 echo 服务器，宿主侧 `scripts/tcp_server_acceptance.py --command tcpapi` **8/8**（`TCPAPI accepted fd=1 from 10.0.2.2:56195` → 回显 18 字节逐字节一致）；`--command tcpserve`（裸 tcp_* 路径）同样 8/8。
  **未实现**：`linux_compat` 里的 socket 号仍未在 syscall 表注册（`socket/bind/listen/accept/connect/sendto/recvfrom` 目前只能由内核调用；用户态要等 syscall 表补上），`getsockname/getpeername/setsockopt` 对 TCP 只返回记录值。
  **未实现**：多个待 accept 连接的排队语义（backlog 只体现为槽位上限）、SACK、窗口缩放、时间戳、零窗口探测、拥塞控制（固定停等窗口）、MSS 协商、乱序队列深度 > 1；`linux_compat` 里的 socket 号仍未注册（返回 ENOSYS）。

### Phase 6: 专业 ISO 发布
- ✅ **双引导 ISO**: Limine (BIOS 已实测启动；UEFI 路径未实测)、`/boot` `/system` `/EFI` 布局、内核双份、initrd、manifest.json
- ✅ **ISO 大小**: ~11.5 MB

### Phase 7: Shell 与应用
- ✅ **Shell**: 真实 `ls/cd/pwd/cat/ps/kill/df/mount/umount/echo/help/clear/startgui/reboot/shutdown/cpus/smptest/ping/netstat/tcpecho/tcpsend/tcpstat/kbdstat`；`ls` 走 VFS readdir、`cat` 走 VFS 读、`df` 读真实挂载与空间、`mount` 走统一 VFS、`kill` 真从 EEVDF 队列摘除实体；实测输出见下"验证记录"
- 🟡 **键盘输入**: **IRQ1 已接通**（IOAPIC 级联 ExtINT + `moos_irq_register(0x21)` + 8042 命令字节开 IRQ1），scancode 进 64 字节环形缓冲，`mbos_kbd_poll()` 先从环取再由端口轮询兜底——这样"内核忙时到达的按键"不再丢（此前 8042 单字节缓冲会丢掉）。但实测本机 QEMU 11.1 上 IRQ1 **只送前 ~2 个中断就停**（`kbdstat` 显示 `vector0x21=2`、ring 只收到 1 个 scancode），所以**轮询仍是主力**，中断只当增强。Shell 新增 `kbdstat`。

---

## 🧪 验证记录（2026-10-14 复测：存储四控制器 + 网络收发 + TCP + FAT32 目录操作）

```
[selftest] 13/14 passed（仅既有的 moos_intr 判定问题）
[blk] ATA / ahci / nvme / virtio-blk 四个控制器全部上线，驱动器号 0/4/8/9/10 互不重叠
[fat32] mount drive=0|4|8|10 ... write self-test PASS + ops self-test PASS (3/3)
      → 宿主 verify_fat32_write.py 各 checks=17 ok=17（含 rename/unlink/rmdir 与孤儿簇链检查）
[ext2] selftest PASS (hello=0xccc16a4b big=0x7beec92a nested=0xa31a867d sparse=0x8f7abe7d double=0xeb83dd9b)
[net] selftest ping 10.0.2.2 -> reply in 1288 us / sutureos# ping 10.0.2.2 → 3/3 应答
      → 宿主 scripts/net_probe.py: ALL PASS 22 checks, 0 failed
[TCP] scripts/tcp_acceptance.py：echo 7/7（双向逐字节）、bulk 6/6（3000 字节 3 段）
```

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

宿主侧校验：`scripts/mkfat32_testimg.py` 自建 superfloppy FAT32 镜像（70544 簇、FAT1==FAT2、FSInfo 合法、根簇标记为 EOC）与真实 `.deb`（ar + control.tar.gz + data.tar.gz），并用独立解析器逐字节校验；只读启动（无写测试时）前后整盘 CRC32 一致。

> **操作教训（踩过的坑）**：`-serial file:` 是带缓冲的，**强杀 QEMU 会丢掉日志尾部**——曾因此误判"AP 没起来/shell 没启动"。读日志前请用 QEMU monitor 发 `quit` 优雅退出（或用 `-serial stdio` 重定向），再用 `Get-Content` 读取。

**终端 / 电源路径验收（QEMU `-vga std`，monitor `screendump` 截图后量化）**：

```
[term] selftest: 25 px visible at cell(0,0) (expect 25)     <- 渲染通路自检（画 H，数 25 个白像素）
[term] flanterm arena 316 KiB of 1024 KiB used
[term] flanterm ready 113x48 on 1024x768@32bpp
[stitch] selftests: 12/13 passed
[fat32] write self-test PASS (SUTURE.TXT, 32 bytes)
[SMP] AP 1 online (lapic=1) / online=2/2
```
- 截屏量化：1024x768 画面里 **493 行有内容**、约 3 万个白色像素；目视为黑底白字的完整英文启动日志 + `sutureos#` 提示符 + 光标块（修复前：整屏只有 10 个 `[` 字形）。
- 关机：`shutdown` → `[power] ACPI S5: outw(0x604, 0x3400)` → monitor 报告 **`VM status: paused (shutdown)`**。
- 重启：`reboot` → `[     ] Trying chipset reset port 0xCF9` → 串口日志出现**第二次完整启动**（boot marker 计数 1 → 2）。
- 宿主侧 FAT32 写验收在同一版本上复跑：`checks=10 ok=10 fail=0`。

**写路径验收（`scripts/verify_fat32_write.py`）**：

```
[fat32] write self-test PASS (SUTURE.TXT, 32 bytes)
  [ok] whole-image CRC32 changed (write reached the medium)  (1F9AD269 -> 4CE352AE)
  [ok] FAT1 == FAT2
  [ok] root cluster marked allocated  (fat[2]=0xfffffff)
  [ok] SUTURE.TXT exists with correct size  (size=32)
  [ok] SUTURE.TXT content byte-for-byte  (b'SutureOS FAT32 write self-test\n\x00')
  [ok] SUTURE.TXT cluster not the root cluster  (first=7)
  [ok] HELLO.TXT unchanged (83 bytes)
  [ok] README.TXT unchanged (149 bytes)
  [ok] HELLO.DEB unchanged (746 bytes)
  [ok] FSInfo free count == measured free clusters  (fsinfo=70538 measured=70538)
=== RESULT: PASS ===  checks=10 ok=10 fail=0
```

## 🖥️ 终端、GUI 应用、多核与 ext2

### 默认进终端，`startgui` 才进 GUI
引导成功后屏幕是**黑底白字的终端**（移植 flanterm，BSD-2，见"已知问题"第 7 条），GUI 必须显式输入 **`startgui`**。

GUI 里现在有**三个真实应用**（`kernel/gui/apps.c`，用公开的 `gui_create_window()` + `gui_fill_rect/gui_draw_str` API 构建）：

| 窗口 | 内容 |
|------|------|
| **Task Manager** | 实时：CPUs online、PMM 空闲页/MiB、堆空闲 KiB、Uptime、FPS、窗口数、CPU busy 进度条 |
| **Calculator** | 键盘驱动：`0-9` `+ - * /` `=` `c`，算式与结果显示在客户区 |
| **About SutureOS** | 版本、14 个上游项目、flanterm(BSD-2) 与 GPL-3.0 |

GUI 键盘：`Tab` 切焦点、方向键移动窗口、`r` 重绘、`q`/`ESC` 退出回终端（退出会恢复终端并清屏）。

### 多核：AP 真正参与内核执行
- IDT 补齐 **0xE0（重调度 IPI）/0xE1（TLB shootdown）/0xFF（LAPIC 假中断）** 三扇门（此前 not-present，收到即 #NP）；
- 每个 AP 用**自己的 LAPIC 定时器**（向量 0x20，周期模式）计时，`cpu_local.jiffies` 独立计数；
- **SMP 工作队列**：`smp_submit_work()` 投递内核工作并**发 IPI 唤醒空闲核**，各核在空闲循环里 `smp_drain_work_here()` 取活执行，`cpu_local.work_done` 计数；
- Shell 新增 `cpus`（每核 jiffies / IPI / 执行工作数 / 空闲次数 + 实测时钟频率）与 `smptest [n]`。

实测：
```
[SMP] work selftest: APs finished 32 of 32 before the BSP helped
[SMP]   cpu0 (BSP): same-window work_done=0  jiffies=6089
[SMP]   cpu1 (AP):  same-window work_done=32 jiffies=6233
[SMP] work selftest PASS (an AP executed kernel work)
```
范围：AP 现在执行**内核工作项 + 定时器 + IPI**，但仍不进 task.c/eevdf 的上下文切换（两套调度器共用 context，合并前让 AP 切任务会重演 #UD 洪水）。

### ext2（只读，真实驱动）
`kernel/fs/ext2.c`：superblock/BGDT 校验、单/双/三级间接块、稀疏空洞、readdir/stat/read、越界钳制与告警；`ext2_vfs.c` 接入统一 VFS，写操作返回 EROFS(→EPERM)。挂第二块 IDE 盘即可：
```powershell
py -3 scripts\mkfs_ext2_testimg.py     # build/ext2.img (1KiB 块) + 4KiB 变体 + 三重间接夹具
# QEMU 加 -hdb build/ext2.img；kmain 自动 ext2_mount(1,0) + vfs_mount("1","/mnt/ext2","ext2") + 自检
py -3 scripts\verify_ext2_read.py build\ext2.img     # 宿主侧独立验收
```
实测：`[ext2] mount blocksz=1024 blocks=4096 inodes=64 groups=1 rev=1`、`[ext2] selftest PASS (hello=0xccc16a4b big=0x7beec92a nested=0xa31a867d sparse=0x8f7abe7d double=0xeb83dd9b)`（含 300 KiB 双间接文件与稀疏空洞）。宿主侧另有 91/91 + 20/20（含第三方 mke2fs 镜像）与 38/38 解析校验。

### 存储栈：ATA / AHCI(SATA) / NVMe / VirtIO-blk

四个控制器驱动都是真实实现（各自的源码头部记录了参考的上游项目与许可证），共用 `kernel/drivers/block.h` 的 `struct blk_device` 接口：

| 驱动 | 文件 | 驱动器号 | 传输方式 | QEMU 设备 |
|------|------|----------|----------|-----------|
| ATA PIO | `block.c`（内建） | 0/1 | PIO 轮询、LBA28 分块 | `-hda` / `-hdb` |
| AHCI/SATA | `ahci.c` | 4..7 | 命令列表 + FIS + PRDT DMA，纯轮询（PxIE=0） | `-device ahci,id=ahci -device ide-hd,drive=d,bus=ahci.0` |
| NVMe | `nvme.c` | 8 | admin + IO 队列对、PRP、相位位轮询完成 | `-device nvme,drive=d,serial=...` |
| VirtIO-blk | `virtio_blk.c` | 10 | legacy 0.9.5 BAR0 + split virtqueue（静态 16 KiB DMA 环） | `-device virtio-blk-pci,drive=d` |
| RAM 盘 | `block.c` | 9 | 内存 | — |

DMA 目标一律是静态 `.bss` 缓冲（物理==虚拟，1 GiB 恒等映射内），不经过内核堆，因此后续 PMM/堆再初始化也不会把环搬走。**驱动器号互不重叠**（`block.h` 有表），kmain 依次探测并把第一个带 FAT32 卷的设备挂到 `/mnt/fat`。

验收方式对四个控制器完全一致——把同一张 FAT32 测试镜像挂上去，内核自动挂载 + 写自检，再由**宿主侧**独立解析镜像：

```powershell
py -3 scripts\mkfat32_testimg.py                       # 生成 build/fat32.img
Copy-Item build\fat32.img build\fat32_virtio.img       # 每个控制器一份副本
powershell -NoProfile -File scripts\qemu_boot.ps1 -SerialLog build\serial_virtio.log `
    -ArgFile build\qemu_virtio.args -BootWaitSeconds 16
py -3 scripts\verify_fat32_write.py build\fat32_virtio.img build\fat32.img
```

四个控制器逐个实测均为 `=== RESULT: PASS ===  checks=10 ok=10 fail=0`（新文件逐字节正确、旧文件未改动、FAT1==FAT2、根簇仍 EOC、FSInfo 计数与实测一致、整盘 CRC 变化）。四者 + NIC 同时接入的一次启动实测：

```
  drive=10 type=3 name=vda      sectors=71680   (VirtIO-blk)
  drive=8  type=1 name=nvme0    sectors=71680   (NVMe)
  drive=4  type=2 name=sata0    sectors=71680   (AHCI/SATA)
  drive=0  type=4 name=ata0     sectors=71680   (ATA PIO)
  drive=9  type=5 name=ram0     sectors=2048
[fat32] write self-test PASS / [net] selftest ping 10.0.2.2 -> reply in 1293 us
```

### 版本与发布

- 版本号遵循 **SemVer 2.0.0**，唯一来源是 `include/sutureos/version.h`（内核横幅、
  shell 欢迎语、`/etc/motd`、ISO `manifest.json` 都从这里取，`scripts/build_iso.py`
  直接解析该头文件）。发版只改这一处。
- 每次改动都会提交到 `main` 并推送到 <https://github.com/Fanqi89/SutureOS>，同时打一个
  **附注标签** `v<版本号>`，标签说明写清这个版本做了什么；**旧标签不移动、不删除**，
  所以新旧版本在仓库里共存。`CHANGELOG.md` 同步记录。
- 一键发布：`powershell -NoProfile -File scripts\release.ps1 -Message "feat: ..." -NotesFile <说明文件>`
  （已提交过则用 `-PushOnly`）。脚本从 `version.h` 读版本、自动打附注标签、经系统代理
  推送 `main` + `tags`。
- 标签与 Release 的内容规范写在 [`docs/RELEASING.md`](docs/RELEASING.md)：版本号（SemVer 唯一来源）、
  附注标签的固定结构（关键修复 / 新增 / 验证 / **产物（ISO 文件名、大小、sha256）** / 已知限制）、
  Release 标题与正文格式。
- 每个 Release 都上传 **`sutureos.iso`** 附件（历史版本各自保留各自的镜像）：
  <https://github.com/Fanqi89/SutureOS/releases> —— 一键上传用 `scripts\publish_release.ps1`。
- 当前标签：`v0.1.0-alpha`（初始提交）→ `v0.5.0-alpha`（GUI/TCP-IP/多文件系统的第一个大版本）
  → `v0.6.0-alpha`（VMware 兼容修复、TCP listen/accept、套接字 ABI、SquashFS、启动呈现、
  日志环、PS/2 鼠标 + 开始菜单）。
- 本机推 GitHub 必须走 HTTP 代理（直连 TLS 会被重置）：脚本读 Windows 系统代理
  （`HKCU\...\Internet Settings` 的 `ProxyServer`），可用 `-Proxy` 覆盖。

### 本机测试（QEMU / VMware）
```powershell
& "C:\Program Files\qemu\qemu-system-x86_64.exe" -machine pc -m 256 -vga std `
  -display none -serial file:build\serial.log `
  -cdrom build\sutureos.iso -boot d -hda build\fat32.img -hdb build\ext2.img -smp 2 `
  -no-reboot -no-shutdown -monitor tcp:127.0.0.1:45470,server,nowait
```
- `-vga std` 提供 0xFD000000 线性帧缓冲（终端/GUI 才可见）；`-hdb build/ext2.img` 挂 ext2 测试卷。
- **推荐用封装脚本**（自动以 `quit` 收尾、可注入按键与 monitor 命令）：`powershell -NoProfile -File scripts\qemu_boot.ps1 -SerialLog build\serial.log -ArgFile <含额外 -device 的文件> -BootWaitSeconds 16 -Keys "p;i;n;g;spc;1;0;dot;0;dot;2;dot;2;ret"`
- 存储控制器加盘：`-device ahci,id=ahci -device ide-hd,drive=d,bus=ahci.0`（SATA）、`-device nvme,drive=d,serial=sutureos0`、`-device virtio-blk-pci,drive=d`（都要配 `-drive id=d,file=...,if=none,format=raw`）。
- 截图：monitor 里 `screendump build\screen.ppm`；**退出前务必 `quit`**（强杀会丢 `-serial file:` 尾部日志）。
- 输入模拟：`sendkey`，键名如 `ret`/`spc`/`slash`/`tab`/`shift-equal`；键盘是轮询无 IRQ，按键间隔 ≥400 ms。
- **启动呈现（按要求）**：开机 = **滚动的英文日志** → **系统名称横幅** → **干净的纯终端**（提示符可用）→ 输入 `startgui` 进入 GUI 桌面。
  - `boot/limine.conf`：`timeout: 0` + `default_entry: 1` —— **关掉 Limine 自带的菜单/倒计时**，所以屏幕第一眼看到的就是内核英文日志；第二个条目保留 verbose 启动用于调试。
  - `kernel/term/boot_banner.c`（新）：日志滚完后**清屏**并打印系统名横幅（SutureOS / version 0.5.0-alpha / 一行状态提示），随后 shell 接管，提示符 `sutureos#` 出现。
  - 清屏踩过的三个坑（都记在代码注释里）：① **flanterm 不支持 `ESC[2J`（整屏擦除）**，只实现了 `ESC[0J/ESC[1J`——发 `ESC[2J` 被静默忽略，于是日志一直留在屏上；现改成**逐行 `ESC[<row>;1H` + `ESC[2K`** 擦除（与光标位置、帧缓冲几何无关）。② 清屏前必须先 `flanterm_flush()`，否则双缓冲里排队的那半行日志会在清屏之后**画回原来的位置**（VMware 上表现为 banner 周围散落日志碎片）。③ 早期日志是 fbcon 直接画进帧缓冲的，flanterm 的网格是空的，所以 `term_clear()` 现在额外用背景色**擦一遍帧缓冲像素**（`term_wipe_framebuffer()`）。
  - 验收：QEMU `-vga std` + monitor `screendump` → 截图只有横幅/欢迎语/提示符，**无任何残留**；VMware 17.5.2（4 GB 那台）截图同样干净；`startgui` → `[gui] GUI framework init 1024x768` → 三个窗口（Task Manager / Calculator / About SutureOS）→ GUI 主循环。
- **终端/日志自动滚动**：内核控制台每写一行都会进一个 **128 行的环形缓冲**（`kernel/term/log_ring.c`，捕获点在 `console_putc()` 最前面，屏幕或串口坏了也不丢日志），屏幕输出本身始终跟随最新一行（flanterm 滚屏）。
  - 新命令 `log [行数]`（别名 `dmesg`）：打印最近 N 行（缺省 = 一屏少 4 行），**自动滚到日志尾部**，提示符紧跟最后一行——`log 200` 会打印环里全部 128 行并停在最新行。
  - 实测：`log` → 44 行；`log 200` → 128 行（`[selftest][rtl8139] PASS` 开头、`sutureos#` 结尾），串口与屏幕内容一致。
- **GUI 鼠标 + 开始菜单**：启动 GUI 后屏幕上就有白色箭头光标（PS/2 鼠标，轮询）；**点击左下角的 “SutureOS” 名字会向上弹出开始菜单**（Task Manager / Calculator / About SutureOS / 分隔线 / Exit to terminal），点菜单项会把对应应用拉到最前、点窗口标题栏的三个按钮可最小化/最大化/关闭。
  - 驱动：`kernel/drivers/mb_mouse.c`（8042 aux 端）2 开机时就初始化（`kmain`，不是进 GUI 才初始化），循环里逐帧轮询；启动时 8042 配置字节常常是 `0x61`（**aux 时钟关着**），所以初始化里强制清 bit5（开时钟）、置 bit1（aux 中断）并补发 `0xA8`/`0xAE`（开端口 + 开设备）、`0xF6`/`0xF4`（缺省 + 开数据上报）。鼠标字节与键盘共用 8042 输出缓冲，所以 `mbos_kbd_poll()` 不再吃掉 bit5 置位的鼠标字节（否则会当扫描码处理）。
  - 诊断：`mouseinfo`（shell）打印 8042 状态、配置字节、设备 ID（0 = 标准三字节鼠标）与包计数；GUI 里每秒打一行 `[gui] pointer x=.. y=.. buttons=.. pkts=.. obf=.. aux=..` 到串口。
  - 验收：`startgui test` 会把**人工构造的 PS/2 包送进真实解码器**（QEMU 无显示后端时不能注入真实指针事件），逐步移动光标→点开始按钮→菜单弹出→点第一项，串口依次打出
    `pointer selftest: on the start button (12,736)` → `start menu: opened (click 12,736)` → `after the click the menu is OPEN` → `start menu: 'Task Manager' (click 18,612)` → `finished (menu=closed, focus='Task Manager')`，截图里能看到菜单从左下角向上弹出。
  - **未验证**：真实鼠标的移动包路径（自动化环境里 QEMU `mouse_move`/QMP `input-send-event` 与 VMware VNC 的指针事件都到不了客机；但 8042 aux 数据通路本身已由 `mouseinfo` 读到设备 ID 证实）——请在自己的 VM 里移动真鼠标确认，如果不动就跑 `mouseinfo`，看 `packets/obf/aux` 哪一步是 0。
- **VMware**：新建 *Other 64-bit* 虚拟机 → 内存 ≥256 MB、≥2 核 → 光驱指向 `build/sutureos.iso`（可再加两块 IDE 盘分别挂 fat32.img / ext2.img）→ 启动进终端，`startgui` 进 GUI，`shutdown`/`reboot` 走 ACPI；可加串口重定向到文件抓同样的 `[  OK  ]` 日志。
- **VMware 自动化（新）**：`powershell -NoProfile -File scripts\vmware_boot.ps1 -SerialLog build\vmware_serial.log -DiskImage build\fat32_ahci.img -WaitSeconds 60`——在 `build\` 里生成一次性 VM（不碰你在 Workstation 里已打开的虚拟机）、ISO 挂光驱、raw 测试镜像当 SATA 盘挂上、串口写文件、跑完自动关机。实测（VMware Workstation 17.5.2 + 虚拟 SATA 盘）：
  ```
  [ahci] port 0: VMware Virtual SATA Hard Drive sectors=71680 lba48=1 serial=00000000000000000001
  [ahci] port 0: TFD=0x8 not idle after completion (device quirk); transfer accepted (PxCI clear, no ERR/DF)
  [fat32] mount drive=4 ... write self-test PASS + ops self-test PASS (3/3)
  → 宿主 py -3 scripts\verify_fat32_write.py build\fat32_ahci.img build\fat32.img → checks=17 ok=17
  ```
  三个 VMware 专属坑（脚本已处理，记在这里免得重踩）：① VMware 不认 raw 镜像，必须是 VMDK——用一个**只有描述符**的 VMDK 把 raw 文件当 FLAT extent（`RW <扇区数> FLAT <file> 0`），好处是 guest 的写直接落在 raw 文件里，宿主侧校验脚本不用改；② FAT32 超级软盘镜像自带 `0x55AA` 引导签名，不加 `bios.bootOrder = "cdrom"` 时 BIOS 会先去引导数据盘，guest 挂死且串口日志为空；③ .vmx 的注释符是 `#`，写成 `;` 会让 Workstation 直接报"无法读取虚拟机的配置文件"。

---

## ⚠️ 已知问题（如实记录）

0. **曾经误判过的"FAT32 写缺陷"其实是测试镜像的错**：`mkfat32_testimg.py` 最初忘了把根目录簇（簇 2）在 FAT 里标记为 EOC，于是内核"正确地"把第一个空闲簇（=根目录）分配给了新文件，表现为"payload 写进根目录、目标簇是陈旧数据"。排查时用块层隔离自检证明 **ATA PIO 读/写/写后读全部正确**（`read lba32 head=f8 ff ff 0f…`、`write lba70000 rc=0`、`readback same=1`），再用 `[fat32] dbg raw sec_read(32)` 看到 FAT[2] 在盘上就是 0，才定位到镜像生成器。两个脚本现已加入断言（根簇必须 EOC、FSInfo 必须等于实测空闲簇数），避免回归。教训：**宿主侧解析镜像比任何 guest 自报都可靠**。
1. **`[selftest][moos_intr] FAIL`**：moos 中断自检故意注入异常（`err=0xbad`），处理函数会打印告警并把该自检判为 FAIL——属该自检与框架的判定不一致，非本次引入。
2. **两套调度器并存**：`task.c` 的协作轮转与 `eevdf.c` 的 EEVDF 双轨运行，任务内调用 `sched_yield()` 会切到 EEVDF 循环的陈旧上下文（实测表现为跳到 0x3）。Shell 输入循环因此改为有界 pause 自旋，未再暴露；根治需要合并调度器。
3. **`context.kgs_base` 被两用**：`cinux_context_switch` 把它写进 MSR_GS_BASE，而 EEVDF 用同一字段存放 `sched_entity*`；建议为实体指针另设字段。
4. **（部分修复）IDT 与 AP**：0-47、0x80 以及 IPI 向量 0xE0/0xE1/LAPIC 假中断 0xFF 都已有门（此前 not-present 会 #GP）；`intr.c` 的 0x20-0x2F 通用路径必须发 EOI，否则 8259 ISR 位挂死整条线。AP 现在执行内核工作项 + 定时器 + IPI，但**仍不进 task/EEVDF 的上下文切换**（两套调度器共用 context，见第 2 条）。
5. **（已修复）帧缓冲曾长期不可用**：原因是 (a) `boot.S` 的 multiboot1 视频字段写在了偏移 12..27，而规范要求放在跳过的 a.out kludge 区之后（32/36/40/44），固件因此读到"0x0 分辨率"只给 VGA 文本模式；(b) `limine.c` 把 multiboot1 的 MBI 当 Limine tag 流解析，永远得到 0；(c) 线性帧缓冲位于 1 GiB 以上（0xFD000000），开机恒等映射覆盖不到。三点均已修复（正确的头字段 + 规范解析 + `mmu_map_mmio_identity`），实测 `1024x768 32 bpp pitch=4096`。
6. **键盘无 IRQ**：只轮询 8042 单字节缓冲，快速输入丢键（自动化测试请把按键间隔设为 ≥400 ms）。
7. **flanterm 上游缺陷（已在本仓库标注修复）**：`canvas == NULL` 时使用的 `plot_char_*_uncanvas()` 把默认**前景色**也取成 `default_bg`，导致未指定 SGR 的普通文本是黑底黑字。已在 `fb.c` 两处修正并加 `SutureOS fix` 注释（BSD-2 允许修改，许可证头保持原样）。
8. **（已修复，影响面最大的一个）PMM 把自己的页标志数组分配了出去**：`moe_pmm_init()` 用区域开头 1 字节/页 存标志，而 `moe_pmm_alloc()` 从第 0 页开始扫描——于是 `kmain` 的 `moe_pmm_alloc(2048)` 把**标志数组本身**当成了内核堆（8 MiB 堆 == 标志数组），此后每次分配/释放都把 `0x01/0x00` 写进活堆：堆头、flanterm 上下文（`text_fg` 被写成 `0x00000001` → 文字不可见、只剩残留光标块）、以及之后分配的 AP/任务栈。已修：`moe_pmm_init()` 把标志占用的页标记为 `TAKEN` 并从空闲计数里扣除，自检也改成断言"标志页绝不出借"（防回归）。同类症状的"AP 栈落进内核镜像"一并消失。
9. **系统时钟改用 LAPIC 定时器（PIT 中断在这台机器上送不到 CPU）**：QEMU `-d int` 追踪显示内核重映射 PIC 之后**没有任何 IRQ0**——该机器带 IOAPIC、上电即 APIC 模式，8259 的 INTR 并不走 LINT0（试过 IMCR 切 PIC 模式也无效）。因此系统节拍由 BSP 的 LAPIC 定时器提供（与 AP 同机制），并**实测频率**（`cpus` 命令会打印，当前约 57–100 Hz，标定基于 8254 计数器、仍偏保守）。PIT 仍被编程为 100 Hz 并保留自检。这类"时间基准"问题此前被掩盖，是因为 `g_tick_count` 从未真正推进过（FPS 显示的是初值 60）。
10. **`smp_get_cpu_id()` 之外的宽泛地址守卫已清理**：`fbcon.c`/`gui.c`/`limine.c` 里"帧缓冲 ≥1 GiB 就拒绝"的旧判断都删了（LFB 现在由 `mmu_map_mmio_identity()` 显式映射）。
11. **（已修复）RTL8139 收发曾经完全跑不通——四个叠加的真实 bug**：修复后 `ping 10.0.2.2` 3/3 应答、`net_probe.py` 22/22 通过。按发现顺序：
    - **(a) TSD 门铃**：`rtl_outl/rtl_inl` 曾被写成两个 16 位半字，而 QEMU 只在**32 位**写处理器里实现 TSD 发送门铃，于是写被丢弃；同时写 `0x3000|len` 会置上 `TxHostOwns(0x2000)`，QEMU 认为描述符仍归主机所有而拒绝发送。修正：`include/stitch/io.h` 补真正的 `outl/inl`，TSD 只写长度（OWN=0），`RTL_TSD_TABT` 改为 `1u<<30`。
    - **(b) RX 条目步长少 4 字节**：芯片在每条条目尾部还写 4 字节 FCS，而头里的长度字段**不含**这 4 字节（guest 显存实测：`+0x00: 01 00 44 00` 头、`+0x44: d8 ac 72 f8` FCS、`+0x48` 才是下一条头）。原代码按 `len` 前进，正好落在 FCS 上，把它当成长度 0xf872acd8 的垃圾头 → 走"重新同步"分支跳到写指针，**跳过所有已缓冲帧**：每次开机只能收到 1 个 ARP 应答，ping 永远超时。修正：`head = (off + len + 4 + 3) & ~3`。
    - **(c) ChipCmd.BUFE / CBR 不可信**：写 `RXEN|TXEN` 会清掉 BUFE，QEMU 模型也不再置回；而本机 QEMU 11.1 的 CBR 即使环里躺着 612 字节数据也读回 0（monitor 实测 `CAPR=0 CBR=0 ChipCmd=0`）。原来基于 CBR 的"有没有数据"判断因此每轮都误判，且读-改-写 ChipCmd 会把 TXEN 一起清掉（之后连发包都停了）。修正：环形缓冲在 init 时清零、只有芯片会写它，于是**用"读指针处的头是否 ROK 且长度合法"**判空/判满，完全不依赖设备寄存器；ChipCmd 只写常量，不再读-改-写。
    - **(d) ICMP id/seq 字节序**：`rest_of_header` 是 u32，小端下**低半字才是 identifier**，接收路径按此解析；发送路径却把 id 放进高半字，于是每一个 echo 应答都被 `id == NET_ICMP_ID` 判掉——netstat 里 `icmp rx=1` 明明收到了应答，ping 还是超时。修正：`((u32)htons(seq) << 16) | htons(NET_ICMP_ID)`。
    残留限制：RX 的"条目写完整了吗"用有界 settle 自旋近似（QEMU/TCG 够用；真机若头先于负载可见，理论上仍有窄窗口）。TCP 未实现。
12. **多驱动共用块层的驱动器号只能靠约定**：`blk_register()` 不拒绝重复的驱动器号，`blk_find_by_drive()` 返回最后注册的那个。现行分配见 `block.h`（ata0/1=0/1、AHCI=4..7、NVMe=8、ram0=9、vda=10），四个控制器同时接入的实测输出见"验证记录"。
13. **`scripts/qemu_boot.ps1`（新）**：本机自动化跑 QEMU 的封装——工作目录固定为项目根（QEMU 打不开非 ASCII 绝对路径）、monitor 走 **QMP**（干净 JSON，无 readline 回显）、支持按顺序 `sendkey` 注入与任意 monitor 命令（`i /4 0xc100`、`xp /96bx ...` 这类寄存器/内存取证就是靠它做的），并且**始终以 `quit` 收尾**再读日志（强杀会丢 `-serial file:` 尾部，曾因此误判 AP 未上线）。
14. **TCP 是"停等 + 只收按序段"的子集**（见 Phase 5）：没有监听队列（`listen/accept` 仍返回 ENOSYS）、没有乱序重组、没有拥塞控制和窗口缩放；`net_alloc_port()` 只查 UDP socket 表，TCP 端口不与它去重。RTO 固定 500 ms × 4 次，失败即如实报错（`[tcp] give up: ...`），不假装成功。
15. **FAT32 只支持 8.3 名字**：因此 rename 是"同目录名字段改写"，跨目录移动与超过 8.3 的名字会被拒绝（实测中 `SUTESTDIR` 这种 9 字符名字会被 `make_raw` 以 `FATFS_EINVAL` 拒绝——不是缺陷，是如实拒绝）；LFN 未实现。
16. **（已修复一半）IOAPIC 从来没有被初始化过**：`arch/x86_64/ioapic.c` 的移植版把地址硬编码成 0xFEC00000，然后因为"地址 ≥1 GiB、不在恒等映射里"直接 `return false`（注释写"deferring to Phase-3"），于是 IOAPIC 一个寄存器都没碰过。这台机器上电即 APIC 模式、8259 的 INTR 接到 IOAPIC pin 0，而该重定向项默认是屏蔽的——**这就是"IRQ0 永远送不到"的真正原因**（系统节拍因此改用 LAPIC 定时器，现在依然如此，两路都用 0x20 会双倍计数，所以 IRQ0 保持屏蔽）。修法：用 `hanos_madt_get_ioapic_addr()` 取地址 + `mmu_map_mmio_identity(0xFEC00000, 0x2000)`（与 LAPIC/帧缓冲/AHCI 同一套路），再把 pin 0 设成 ExtINT 并解除屏蔽，同时把 IRQ1 从 8259 IMR 里放出来。另外 **ExtINT 交付的 IRQ 还必须给 LAPIC 发 EOI**（`hanos_lapic_eoi()`），否则 LAPIC 的 in-service 位一直挂着——实测不发就只有**第一个** legacy IRQ 能进来。残留：IRQ1 实际只送 2 个中断就停（`kbdstat` 可查），因此键盘保持**纯轮询**：我把中断处理写成"只计数、不读 0x60"，因为一旦让中断去读端口就会和轮询读端口的路径打架（实测同一条 27 字符命令只到 18 字符，比纯轮询更差）。所以 IRQ1 目前只是可观测的接线成果，输入仍走轮询。
19. **SquashFS 是 gzip-only 的只读子集**（见 Phase 3）：lzo/lzma/xz/lz4/zstd 镜像在 mount 时被明确拒绝而不是误读；xattr/export 表/目录索引项不解析；单文件超过 4096 个数据块（4 KiB 块即 16 MiB）拒绝；uid/gid 只给出 id 表索引；内核没有压缩器，`squashfs_compress()` 返回 ENOTSUP。
20. **（已修复）VMware 下 SATA 读盘全挂 —— 完成判定太严**：用户 VMware 实测截图里第一次读盘就报 `[ahci] port 0 error: tfd=0x8 serr=0x0 cmd=0x24 (tf not idle after completion)`，接着端口重启重试又撞上 `device busy before cmd=0x24`，最终 `sata0 read failed` → FAT32/ext2/SquashFS 全部挂不上。原因是 **VMware 的 AHCI 模型在命令完成那一瞬间 PxTFD 仍显示 DRQ=0x8**（QEMU 不会），而驱动把"完成但 Task File 未空闲"直接判成失败。已修：判据只认 ERR/DF 与 PxIS 致命位；完成后再给一小段有界等待，若仍显示 DRQ/BSY 就记一次 `(device quirk); transfer accepted (PxCI clear, no ERR/DF)` 并置 sticky 标志，后续命令不再为它付等待。VMware 17.5.2 实测：`VMware Virtual SATA Hard Drive sectors=71680` → FAT32 挂载成功 → write/ops 自检 PASS → 宿主校验 `checks=17 ok=17`；QEMU 侧四控制器回归无变化（不会走到 quirk 分支）。
21. **（已修复）内存大就卡死在 ACPI —— ACPI 表可能落在恒等映射窗口之外**：4 GB 内存的 VMware 虚拟机启动时**卡在 `[SMP] BSP LAPIC timer armed` 之后，`[ACPI]` 一行都不打**（同一配置改成 512 MB 就正常，QEMU 小内存也正常，所以一直没暴露）。原因是固件在 RAM 很大时会把 RSDT/MADT 放到内核恒等映射窗口之外，`arch/x86_64/acpi.c` 直接解引用 → 取指/取数异常后卡死。已修：新增 `acpi_touch()`，读任何 ACPI 结构前先用 `mmu_map_mmio_identity()` 按 2 MiB 恒等映射（RSDT、每个候选表头、匹配到的整张表、MADT 全表），并且**先校验长度再遍历**（`length` 有上下界，避免 `entry_count` 下溢和天量校验和循环）；`madt.c` 的 `p += hdr->length` 也加了"0 长度子表即停"的校验。验证：① QEMU `-m 4096` + 四盘 → `[ACPI] Found 2 enabled LAPIC(s)`、AP online、ext2/squashfs 自检 PASS；② 用与用户 VM 同配置的复刻机（virtualHW 21 / 4 GB / lsilogic+AHCI / e1000 / USB+EHCI / sound / PCI 桥 / hpet）实测 4 GB 全程启动到 `sutureos#`；③ **用户自己的那台虚拟机**用重建后的 ISO 实测启动到 shell（`[ACPI] Found 2 ...` → `AP 1 online` → `[  OK  ] ACPI power management` → `[  OK  ] IOAPIC: 8259 cascade routed` → scheduler → shell）。
22. **VMware 里没有网络**：VMware 默认虚拟网卡是 **e1000**（可选项里没有 RTL8139），而 SutureOS 目前只驱动 RTL8139/8139C+，所以 VMware 下会打印 `[net] init failed: rtl8139 not found on PCI` + `[init] no RTL8139 (start QEMU with -device rtl8139,netdev=n0)`，ping/TCP 都不可用；QEMU 下按 README 的 `-device rtl8139,netdev=n0` 一切正常。要在 VMware 里联网需要补一个 e1000 驱动（未实现）。
17. **`arch/x86_64/acpi.h` 与 `madt.h` 重复定义 `acpi_madt`/`acpi_madt_t`**，任何同时包含两者的翻译单元都编不过（本次在 ioapic.c 里用 `extern` 声明绕开，未合并这两个头）。
18. **FAT32 的 ops 自检需要"干净"的测试镜像**：第二次在同一个盘上开机时，`TMPFILE.TXT → RENAMED.TXT` 会因为目标已存在而**正确地**返回 `FATFS_EEXIST`，于是自检打印 `2/3`（这恰好证明"不覆盖已存在目标"生效）。每次验证前用 `scripts/mkfat32_testimg.py` 重新生成或从 `build/fat32.img` 复制一份。

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