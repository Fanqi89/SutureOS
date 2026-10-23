## 🚀 本次更新 (v0.11.0 → v0.12.0)

**开始"缝合"其他系统的优点。** 第一次不从 VimtuOS 移植——把 **Cinux 的 Intel e1000 网卡
驱动**搬进 SutureOS（VMware 的虚拟网卡，以前完全不支持），并给网络栈加了链路层分发。

### ✨ 新增

- **e1000 驱动**（`kernel/drivers/e1000.c`，移植自 Cinux `kernel/drivers/net/e1000.cpp`）：
  PCI 探测（8086:100e/100f/10d3）、MMIO BAR0 映射、复位、EEPROM 读 MAC、RAL0/RAH0/MTA
  过滤编程、legacy RX 环（32×2 KiB）+ TX 环（8 描述符，EOP|IFCS|RS）、轮询收发。
  **这是这台机器上真正用的 VMware 虚拟网卡。**
- **链路层分发**（`kernel/net/net.c`）：`ll_send()` / `ll_poll_rx()` 在 RTL8139（QEMU
  默认，已验证可用）与 e1000（VMware）之间选择，IPv4/ARP/ICMP/UDP 栈与网卡无关；
  `net_link_name()` 报告当前用的是哪张卡。
- **VMware 测试 VM 加网卡**：`scripts/vmware_boot.ps1` 增加
  `ethernet0.virtualDev = "e1000"`，e1000 路径在真 hypervisor 上被测到。

### ✅ 验证

- **VMware 实机（Workstation）**：
  ```
  [e1000] found at 0:16.0 dev=0x100f BAR0=0xfebc0000
  [  OK  ] e1000: MAC=0:c:29:65:1:af link=1 fd=1 RCTL=0x400800a
  [net] using e1000
  ```
  其余启动不变（AHCI/FAT32 正常、ring3 9/9、userspace 9/9）
- **QEMU `-device e1000`**：探测到卡、读出 MAC，**发包在线上验证**——用
  `-object filter-dump` 抓的 pcap 里有我们的 ARP 请求（dst=ff:ff:ff:ff:ff:ff）和 slirp
  回给本机 MAC 的 ARP 应答
- **QEMU 回归（RTL8139 路径）不变且全绿**：自检 13/14、
  `[net] selftest ping 10.0.2.2 -> reply in 2107 us`、FAT32 写+ops PASS、宿主 17/17、
  `ring3 selftest 9/9`、`userspace selftest 9/9`、AP 在线、鼠标

### ⚠️ 已知限制（明说）

e1000 的**收包路径还没打通**：QEMU 与 VMware 上描述符 0 都拿不到 DD 位、GPRC 恒 0，
但**发送是好的**（TDH/TDT 前进、DD 置位），pcap 也证明帧已经到了卡上。Cinux 笔记里
「模拟器 main loop 需要 vCPU 让出」能解释 QEMU，但解释不了 VMware，说明环设置上还有一处
差异要查。

因此分发顺序是 **RTL8139 优先**（不回归现有 QEMU 联网），e1000 只在「只有 e1000」的机器
（VMware）上使用——目前提供探测、MAC 与发送。

### 📦 下载与体验

- 附件 `sutureos.iso`（13.13 MiB，13,768,704 字节）
- sha256：`1113a9d181436da0a29e3d78de7f458399822066d653c2cd0f74efe804cf8c8d`
- QEMU（RTL8139，联网可用）：
  ```
  qemu-system-x86_64 -cdrom sutureos.iso -m 512 -smp 2 \\
      -device rtl8139,netdev=n0 -netdev user,id=n0
  ```
- VMware：默认网卡就是 e1000，开机日志可见 `[e1000] found ...` 与 `[net] using e1000`

### 🧵 "缝合"脉络（这版开始扩展到 VimtuOS 之外）

| 来源 | 贡献 |
|---|---|
| **VimtuOS** | ext2/SquashFS、用户态 libc 与 ring-3、GDT/TSS/分级中断 |
| **HanOS** | LAPIC / IOAPIC / MADT（`hanos_*`） |
| **Lemis** | 内核堆（`lemis_heap_init`） |
| **MOOS** | 中断框架（`moos_*`） |
| **CoolPotOS** | flanterm 终端 |
| **Cinux** | **e1000 网卡驱动 + 其踩坑笔记（本次）** |

后续可继续搬的候选：Cinux 的 FAT32 LFN、HanOS 的调度器（能解 fork/wait4）、MOOS 的 FAT。
