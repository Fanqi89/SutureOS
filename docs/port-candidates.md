# SutureOS 移植候选侦察报告

- **侦察范围**：`C:\Users\fanqi\Desktop\自研操作系统`（49 文件夹 / 50 仓库，清单见该目录 `清单说明.txt`）→ `C:\Users\fanqi\Desktop\SutureOS`（GPL-3.0，x86_64 单内核）
- **本文性质**：只读侦察。素材库与 SutureOS 源码均未修改，唯一写入的文件就是本文件。
- **许可证判定基准**：一律以**本地 LICENSE 文件**为准（`read` 实读），不采信 README、GitHub 徽章或清单说明的二手描述；与实际不符者单列在第 4 节。
- **行数统计**：`Get-Content .Count`（含空行/注释），用于量级比较，不是 SLOC。

---

## 0. 读取时刻必须先知道的三件事

### 0.1 工作树里已经有并行实现（未提交）

`git status` 在我读取时（本次会话）显示：

```
 M kernel/fs/fatfs.c            (+243 行：LFN 读侧已写)
 M kernel/main/kmain.c          (+46)
 M kernel/net/net.c             (+157)
 M kernel/net/net.h             (+4)
 M scripts/mkfat32_testimg.py   (+55)
 M scripts/verify_fat32_write.py(+5)
?? kernel/drivers/e1000.c       (354 行，新文件)
?? kernel/drivers/e1000.h       (56 行，新文件)
```

也就是说 **★1（e1000）与 ★2（LFN 读）已经有别人在写了**。本报告的价值因此调整为：
① 给出**来源选型与备选**（别人选的未必最优，见 2.1 的对比表）；
② 给出**许可证纠正**（树内 `e1000.h` 把 Cinux 写成 GPL-3.0，实际是 MIT，见 4.4）；
③ 给出**可量化验收与回归陷阱**（尤其 `scripts/net_probe.py` 的正则硬依赖，见 2.1.7）；
④ 指出 **LFN 写入侧仍是缺口**（`make_raw()` 仍拒绝 >8.3，见 2.2.6）。

### 0.2 现有验收脚本对日志行有硬依赖

- `scripts/net_probe.py:26` 正则 `^\[net\] rtl8139 found at 0x(..), irq=(\d+), `，`:86` 失败文案 "no '[net] rtl8139 found at' line"。
- `scripts/tcp_server_acceptance.py:124` 固定 QEMU 参数 `-device rtl8139,netdev=n0`。

任何 NIC 抽象层改动都必须**保留**这一行，或同步改这两个脚本，否则回归会假红。

### 0.3 红线提醒（写进每一节的验收）

用户 2026-10-23 明确的移植红线：**启动方式与 GUI 外观（任务栏/窗口边框/标题栏/配色/字体/指针/开始菜单）冻结，只借功能不借 UI**。所以本报告里所有"GUI 应用"类候选（图片查看器、文本编辑器、窗口管理器）都只能取算法/能力，界面必须用 `kernel/gui/apps.c` 风格自己画。★ 三条都避开 GUI，不需要触碰这条线。

---

## 1. 结论表

工作量口径：**小** = ≤2 新文件 / ≤600 行净新增 / 改 ≤2 处现有调用点；**中** = 需改现有数据通路或 600~1500 行；**大** = >1500 行或需要新子系统。

| # | 候选 | 来源（本地绝对路径） | 许可证 | 工作量 | 收益 | 本轮做? |
|---|------|----------------------|--------|--------|------|---------|
| ★1 | **e1000 网卡驱动** | `自研操作系统\VimtuOS\kernel\e1000_64.cpp`(507)+`.h`(16) | **GPL-3.0** | 小（2 新文件 ≈520 行 + `net.c` 约 10 处调用点） | VMware 默认网卡有网，彻底解决"VMware 下无网络" | **做** |
| ★2 | **FAT32 LFN 读（+删除链标记）** | `自研操作系统\Lnxrm\kernel\fs\fat32\fat32_dir.c`(368) | **GPL-3.0** | 中（`fatfs.c` +150~250 行 + 2 个测试脚本） | 长名文件能 `ls`/`cat`/`cd`，中文名可显示 | **做** |
| ★3 | **AC97 声卡（PCM16 播放）** | `自研操作系统\HBOS\src\ac97.c`(169) | **GPL-3.0** | 小（2 新文件 ≈250 行 + `kmain.c`/`shell.c` 各 1 处） | 系统第一次能出声；验收可量化为 WAV 波形指标 | **做** |
| 4 | NVMe FLUSH / virtio-blk flush | `自研操作系统\Cinux\kernel\driver\nvme\nvme.cpp:328` | MIT | 小（30~60 行） | 掉电不丢盘上数据（当前无 flush 路径） | 第 4 优先 |
| 5 | ATAPI CD-ROM + ISO9660 | `Uinxed-Kernel\drivers\block\ata\pata\atapi.c`(441) + `CoolPotOS\module\iso9660\iso9660.c`(432) | Apache-2.0 / MIT | 中（≈900 行，两块） | 能挂安装光盘、`cat` 光盘里的文件 | 下一轮 |
| 6 | FAT32 **长名创建/rename**（LFN 写） | FatFs `put_lfn` 算法（`PlantOS\src\fs\fatfs\ff.c` 等） | ChaN BSD-1 | 中（`fatfs.c` +200~300 行） | 用户能新建中文/长名文件 | 下一轮（★2 之后） |
| 7 | ext2 写支持 | `XJ380\driver\fs\ext4\ext4.cpp`(1762, C++) 或 `Deshab-OS\CODE\dsk\ext4.c`(737, C) | Apache-2.0 | 大 | 系统卷可持久化（现在 ext2 只读） | 不建议本轮 |
| 8 | USB EHCI（U 盘/键鼠） | `Uinxed-Kernel\drivers\usb\host\ehci\`(1 文件 890) | Apache-2.0 | 大（+HID/hub 报告解析） | 真机外设 | 不建议 |
| 9 | USB xHCI | `XJ380\kmod\xhci\`(5 文件 5058) / `Uinxed-Kernel\...\xhci\`(1712) | Apache-2.0 | 大 | USB3 外设 | 不建议 |
| 10 | HDA 声卡 | `VimtuOS\kernel\hda64.cpp`(1107) | GPL-3.0 | 中-大 | 比 AC97 兼容更好 | 与 ★3 二选一 |
| 11 | virtio-gpu 2D | `Deshab-OS\CODE\UTSM\vmm\virtio_gpu.c`(984) | Apache-2.0 | 中-大 | QEMU 下显示加速 | 收益低（framebuffer 已够） |
| 12 | 图片查看器（BMP/QOI 解码） | `HBOS\src\bmp.c`(67) / `PlantOS\src\plic\qoi.c`(516) | GPL-3.0 / MIT | 小-中 | GUI 能看图（界面须自绘） | 可做（非本轮） |
| 13 | HTTP 下载客户端（ring-3） | `HBOS\src\user\progs\wget.c`(124) / `Deshab-OS\CODE\tools\curl\main.c`(418) | GPL-3.0 / Apache-2.0 | 小 | `wget` 能抓网页 | 可做（非本轮） |
| 14 | ring-3 文件工具（ls/cat/cp/mv/rm/mkdir） | 自写；参考 `VimtuOS\user\busybox\vimtu_dirent.c`(351) | 自写 / GPL-3.0 参考 | 中 | `ushell` 里能操作文件（内核 shell 已有同名命令） | 可做（非本轮） |
| 15 | 文本编辑器 | `VimtuOS\user\apps\edit\edit.c`(604) | GPL-3.0 | 中（**UI 必须自写**） | GUI 里能编辑文件 | 谨慎（红线敏感） |
| 16 | 包管理器 | 已有 `kernel/pkg/deb.c`+`inflate.c`；参考 `VimtuOS\user\dpkg\dpkg.c`(470) | GPL-3.0 | 小-中 | .deb 安装已有基础，仅缺 CLI 体验 | 低优先 |
| 17 | ELF 加载器 | **无需移植**：`kernel\syscall\loader.c`(123) 已在，只是启动路径没接线 | — | 小（接线 ≈80 行） | 能跑静态 ELF | 做接线，不做移植 |
| 18 | 计算器 / Task Manager | 已有：`kernel\gui\apps.c`(269) | — | — | — | 不做 |
| 19 | 窗口管理器增强（拖动已做）/剪贴板/缩放 | 只允许借功能：`XJ380`/`VimtuOS` 桌面层 | — | — | — | 不做（红线：禁搬 UI） |

**★ 三条 = e1000（GPL-3.0，小）、FAT32 LFN 读（GPL-3.0，中）、AC97 声卡（GPL-3.0，小）。三条许可证全部可直接融合，且都不触碰 GUI 红线。**

> **对 ★1 的重要补充**：树内已有 `kernel/drivers/e1000.c`(354)+`e1000.h`(56)（未提交，标注来源 Cinux）。若决定沿用这份实现，则 ★1 的**剩余工作只剩**：① 许可标注更正（Cinux 是 MIT，不是 GPL-3.0）；② 与 `net.c` 的 NIC 抽象/双网卡回退；③ 验收。此时工作量从"小"降到**"极小（改 2 个文件 + 补验收）"**。VimtuOS 那份的价值在于"同许可 + 纯 C + 已被同作者的 QEMU/VMware 场景验证过"，可作为**对照实现**或替换候选。

---

## 2. ★ 候选详解

### 2.1 ★1 e1000 网卡驱动

#### 2.1.1 为什么必须做

VMware 的虚拟网卡选项里**没有 RTL8139**，默认就是 e1000；SutureOS 只有 RTL8139 驱动 → 在用户实际使用的 VMware 上**完全没有网络**（这是 v0.6.0 release notes 里自己写下的已知限制）。QEMU 侧两种网卡都有，所以这条既能修 VMware，也能让 QEMU 验收保持双 NIC 覆盖。

#### 2.1.2 来源选型（逐个核对过本地文件）

| 候选 | 路径 | 行数 | 许可证（本地 LICENSE 实读） | 结论 |
|------|------|------|------------------------------|------|
| **VimtuOS** | `VimtuOS\kernel\e1000_64.cpp` + `e1000_64.h` | 507+16 | **GPL-3.0**（`VimtuOS\LICENSE`） | **首选**：纯轮询、无中断、无 C++ 运行时、对外仅 8 个薄函数（init/send/recv/mac/link + 3 个计数器）、同作者同许可 |
| Cinux | `Cinux\kernel\driver\net\e1000.cpp`+`e1000.hpp`+`e1000_config.hpp`+`e1000_layout.hpp` | 217+168+73+158 = 616 | **MIT**（`Cinux\LICENSE`，Copyright (c) 2026 Charliechen114514） | 备选：C++ class + PciBus/mm 依赖，要重写 6~8 处；**但它附带的踩坑笔记更值钱**（见 2.1.5） |
| na-kernel | `na-kernel\modules\drivers\net\e1000\e1000.c` | 556 | GPL-3.0 | 备选：IRQ+MSI+`netdev`+`dma_sync_*`+klibc，剥离约 1/3 |
| Mini-OS | `Mini-OS\v2-c-kernel\src\drv\e1000.c`(+`e1000.h`/`e1000_netif.c`) | 608+56+76 | MIT | 备选：纯 C 轮询，但 `#include` 了它整条栈（ip/udp/icmp/dhcp/netsock），剥完约 350 行可用 |
| XJ380 | `XJ380\kmod\e1000\e1000.cpp`+`.h` | 494+141 | Apache-2.0 | 可用但 C++，与 Cinux 同档 |
| Uinxed-Kernel | `Uinxed-Kernel\drivers\net\ethernet\intel\e1000.c` | 1016 | Apache-2.0 | 依赖 Uinxed HAL/中断框架，剥离成本高 |
| Deshab-OS | `Deshab-OS\CODE\DKM\e1000\e1000.c` | 829 | Apache-2.0 | 依赖 DKM 模块框架 |
| MCubeAI-OS | `MCubeAI-OS\kernel\net\e1000.c` | 282 | MIT | 太薄（AI 生成、11 个提交），只作对照 |
| CoolPotOS | `CoolPotOS\module\e1000\e1000.c` | **10** | MIT | **是桩，不是驱动** |
| xbook2 | `...\drivers\network\e1000\e1000.c`+`e1000_hw.c` | 1772+**5480** | MIT | 32 位时代的 Intel 官方 `e1000_hw.c` 移植，体量等于重写 |
| DragonOS | `DragonOS\kernel\src\driver\net\e1000e\*.rs` | Rust | **GPL-2.0** | **禁用**（GPL-2 与 GPL-3 不兼容） |
| ReliefOS | 只有 `ReliefOS\tools\tests\e1000_init_test.c` + `tools\test_e1000.py` | — | Apache-2.0 | **本地无驱动源码**（`kernel/` 是空子模块） |

#### 2.1.3 首选源码的关键函数（VimtuOS）

`VimtuOS\kernel\e1000_64.cpp`：

| 行 | 函数 | 作用 |
|----|------|------|
| 200 | `e1000_pci_find()` | 0xCF8/0xCFC 扫描，认 0x8086 + id 白名单（0x100E 82540EM 等） |
| 246 | `e1000_eeprom_read(addr,&out)` | EERD 轮询读 MAC（EEPROM 前 3 个字） |
| 256/265 | `e1000_mac_valid()` / `e1000_read_mac()` | MAC 合法性判定 + 读入 `g_mac[6]` |
| 296 | `e1000_setup_rings()` | 32×16B RX + 32×16B TX legacy 描述符环，每 RX 项 2048B 缓冲 |
| 317 | `e1000_init64()` | CTRL.RST → IMC=0xFFFFFFFF+读 ICR → **清 MTA 128×32bit + 清 FCAL/FCAH/FCT/FCTTV** → RCTL/TCTL → RAL/RAH → 环基址 → CTRL.SLU |
| 465 | `e1000_send64(frame,len)` | 取描述符 → 拷数据 → `cmd=EOP\|IFCS\|RS` → 敲 TDT → 轮询 DD |
| 489 | `e1000_recv64(buf,max)` | **先读 RDH**（关键，见 2.1.5）→ 逐项查 DD/EOP → 拷出 → 归还 RDT |
| 516 | `e1000_selftest64()` | IMS 改读回 + 环地址回读 + MAC/RCTL 合理性 |

#### 2.1.4 依赖与 SutureOS 的替换关系（逐个对上）

| VimtuOS 依赖 | 头 | SutureOS 对应 | 适配动作 |
|--------------|----|----------------|----------|
| `outl()/inl()` | `kernel/port.h` | `include/stitch/io.h:40/45` | **同名同语义，零改动**（连"必须 32 位 dword 访问"的注释都一样） |
| `page_alloc_64()` | `kernel/mem_64.h` | `moe_pmm_alloc(1)`；或直接 static `.bss` | 建议照 `kernel/drivers/rtl8139.c:118-127` 用 `.bss`（内核恒等映射低 1 GiB ⇒ 物理==虚拟，注释见 `rtl8139.c:338`） |
| `phys_to_virt_64` | 同上 | 恒等，直接当指针 | 删掉 |
| `dbg64_*` | `kernel/debug64.h` | `console_printf`（`<stitch/console.h>`） | 换打印 |
| BAR0 当指针 | — | 需先 `mmu_map_mmio_identity(bar0, 0x20000)`（`arch/x86_64/mmu.h`） | **必须加**：QEMU i440fx 给 e1000 的 BAR0 是 32 位 MMIO（典型 `0xfebc0000` ≥ 1 GiB），不映射直接写就 `#PF`（这正是 LFB/IOAPIC 踩过的坑） |
| C++ 语法 | `.cpp` | `scripts/build.py:224-226` 只 glob `*.c`/`*.S` | **改名为 `.c` 并把 `bool`→`int`、`nullptr`→`0`、`enum X : uint32_t`→`#define`**（该文件没有 class/模板/new，机械转换约 30 分钟）；或在 build.py 加一条 `-x c++` 规则（不推荐：会给内核引入 C++ 语义） |

#### 2.1.5 必须继承的四条硬经验（来自 Cinux 的笔记，MIT 文档可直接引）

`Cinux\document\notes\2026-06-26-f5-m6-e1000-rx-timer-fix.md` 等三篇记录的头号问题是：

1. **纯内存轮询收不到包**：QEMU 的 e1000 设备模型只在"客户机触发 trap"时推进收包（MMIO 访问/中断/`hlt`）。所以 `e1000_receive()` 必须**先读 RDH** 制造一次 MMIO trap，否则 `GPRC=0、RDH=0` 而包其实已经在线上。症状是"网卡初始化成功但一个包都收不到"。
2. **RDT 要写 N-1 作哨兵**，让 tail 永不等 head。
3. `RCTL = EN(bit1) | SECRC(bit26) | BAM(bit15) | UPE(bit3)`——注意 **EN 是 bit1 不是 bit0**（RTL8139 的习惯在这里会写错）。
4. TX 描述符必须 `EOP|IFCS|RS`，门铃是写 TDT；软复位后**必须清 MTA 与流控寄存器**（残留值会让设备按错误哈希过滤丢包）。

配套笔记文件（若要用 Cinux 代码或写注释引用，都是 MIT，可用）：`Cinux\document\notes\2026-06-26-f7-net-l2-e1000-ping.md`、`2026-06-30-f7-m4-b3-udp-e1000-tx-finale.md`、`Cinux\document\todo\f5-drivers\05-e1000.md`。

#### 2.1.6 与 SutureOS 现有 net 层的接口差异

现状是 **net.c 直接写死 RTL8139**，没有 NIC 抽象（`kernel/net/net.c`）：

| 行 | 现有调用 | 语义 |
|----|----------|------|
| 993 | `rtl8139_init()` | 初始化，返回 `RTL8139_ERR_*` |
| 1009 | `rtl8139_get_mac(net_local_mac)` | 取 MAC |
| 359 / 392 / 491 / 714 | `rtl8139_send_frame(frame,len)` | 发一帧（ARP / ICMP / UDP / IPv4） |
| 1054 | `rtl8139_receive(cb)` | 排空 RX，回调逐帧（**回调式，非缓冲区式**） |
| 1061 | `rtl8139_rx_drops()` | 驱动侧丢包计数 |
| 1018 / 1339 | `rtl8139_io_base()` / `rtl8139_irq()` | 只用于打印 |

建议新增 `kernel/drivers/net_nic.h`（6 个函数：`nic_init/nic_present/nic_get_mac/nic_send_frame/nic_receive/nic_rx_drops` + `nic_name()`），`net.c` 里改成"先试 e1000，再退 rtl8139"：

```c
/* net.c 里替换 993 行 */
if (e1000_init() == E1000_ERR_NONE)      { g_nic = NIC_E1000;  }
else if (rtl8139_init() == RTL8139_ERR_NONE) { g_nic = NIC_RTL8139; }
else { console_printf("[net] init failed: no supported NIC\n"); return -1; }
```

e1000 的 RX 是"拷贝到调用方缓冲"模型（`e1000_recv64(buf,max)`），SutureOS 要的是回调式，**两行适配**：

```c
static int e1000_receive(void (*fn)(const uint8_t*, uint32_t)) {
    static uint8_t buf[2048];
    int n, frames = 0;
    while ((n = e1000_recv64(buf, sizeof buf)) > 0) { fn(buf, (uint32_t)n); frames++; }
    return frames;
}
```

#### 2.1.7 移植步骤草案

1. 新建 `kernel/drivers/e1000.h`：错误码 + `e1000_init/present/get_mac/send_frame/receive/rx_drops/link/io_base/irq`，注释写清来源与许可（VimtuOS, GPL-3.0，或 Cinux, **MIT**——按实际取用的那份写）。
2. 新建 `kernel/drivers/e1000.c`：VimtuOS 那份改名去 C++；BAR0 走 `mmu_map_mmio_identity()`；DMA 缓冲改 static `.bss`（`aligned(16/4096)`）。
3. `net.c`：加 NIC 选择逻辑 + `nic_*` 薄封装（保留 `rtl8139 found at` 那行不变，新增 `[net] e1000 found at 0x%llx, mac=...`）。
4. `kernel/main/kmain.c`：`net_init()` 的调用点不动（选择在 `net_init()` 内部完成）；若要做启动自检，加 `st_e1000_test()` 到 `kernel/main/selftest.c`（同 `st_rtl8139_test()` 的写法）。
5. 打印：`[e1000] pci 0:3.0 id=8086:100e bar0=0x... mmio=ok` / `[e1000] mac=..:..:..:..:..:.. link=up` / `[e1000] init ok rx_desc=32 tx_desc=32`（沿用 VimtuOS 的格式，验收按行 grep）。
6. 回归：`-device rtl8139` 下一切照旧；`net_probe.py` 若因新增行误判，只放宽正则、不改断言。

#### 2.1.8 验收指标（可量化）

| 环境 | 命令 | 通过判据 |
|------|------|----------|
| QEMU + e1000 | `qemu-system-x86_64.exe -machine pc -m 256 -display none -serial file:build\serial-e1000.log -cdrom build\sutureos.iso -boot d -hda build\fat32.img -netdev user,id=n0 -device e1000,netdev=n0` | 串口出现 `[e1000] init ok`；shell `ping 10.0.2.2` → **3/3 reply**；`netstat` 里 `tx_frames>0 && rx_frames>0` |
| QEMU + RTL8139（回归） | 同前，`-device rtl8139,netdev=n0` | `ping 3/3`；`scripts/net_probe.py` **22/22** 不退步 |
| VMware | `powershell -NoProfile -File scripts\vmware_boot.ps1 -SerialLog build\vmware_serial.log -WaitSeconds 60` | 串口出现 `[e1000] mac=` 与 `[net] selftest ping 10.0.2.2 -> reply in <n>`（**这是当前完全缺失的能力**） |
| 寄存器佐证 | 内核打印 `GPRC/GPTC/TDH/TDT/RDH/RDT` 回读 | TX 后 `TDT` 前进、`GPTC>0`；RX 后 `GPRC>0`、`RDH!=RDT` |
| TCP 层 | `python scripts\tcp_acceptance.py`（宿主 echo 服务器） | echo **7/7**、bulk **6/6**，与 RTL8139 时一致 |

#### 2.1.9 工作量

**小**：2 个新文件（≈520 行）+ `net.c` 约 10 处调用点 + `selftest.c` 1 处 = **4 个文件**。树内已有 354+56 行的实现（来源标注 Cinux），本报告的差异主要是**许可标注更正**与 **NIC 抽象**是否要做的取舍。

---

### 2.2 ★2 FAT32 长文件名（LFN）读支持

#### 2.2.1 现状（精确到行）

`kernel/fs/fatfs.c`（原 1273 行，现树内已 +243）原生只认 8.3：

- `#define ATTR_LFN 0x0F`（:29），三处 `continue` 跳过 LFN 项（原 :346 / :976 / :1117）。
- `make_raw()`（:480-492）：`base_len > 8 || 后缀 > 3` → 返回 `FATFS_ENAMETOOLONG`（所以**建不出长名**）。
- `dir_find()`（:493）按 11 字节原始名比较；`resolve_component()`(:587) / `path_resolve()`(:613) 逐分量走 8.3。

**树内已有（未提交）**：`lfn_checksum()`(:341)、`lfn_is_entry()`(:349)、`lfn_reset()/lfn_feed()`(:353/361)、`lfn_assemble()`(:384)、`name_eq_ci()`(:399)、`dir_find_lfn()`(:413)、`dir_mark_lfn_chain()`(:453)，以及 `struct lfn_acc`(:46) 挂在目录游标上(:82)。**= 读侧 + 删除/改名时的链标记已实现**。

**仍是缺口**：**LFN 写入（创建/rename 生成别名短名）**，即 `make_raw()` 仍拒长名，`fatfs_mkdir/create` 不会写 LFN 链。这是本节的真实剩余工作。

#### 2.2.2 来源选型

| 候选 | 路径 | 行数 | 许可证 | 可用性判定 |
|------|------|------|--------|------------|
| **Lnxrm** | `Lnxrm\kernel\fs\fat32\fat32_dir.c` + `fat32_meta.c`(849) | 368 (+849) | **GPL-3.0**（`Lnxrm\LICENSE`） | **首选**：素材库里唯一"完整 + 校验和 + 别名回退 + 断点续扫 + 注释写清每个坑"的 LFN 重建实现，可直接抄 |
| SpiritFoxOS | `SpiritFoxOS\recovered\fs\src\fat32.c:1016-1200` | 1528 | **无 LICENSE 文件**（见 4.3） | 只能用算法：与 SutureOS `fatfs.c` 同源祖先，但 `fat32_populate_dir()` 里**没有校验和校验**（会把残留链当长名） |
| HBOS | `HBOS\src\fat32.c:409-440` | 911 | GPL-3.0 | 极简版（`ch<128` 才收，纯 ASCII），适合当"最小可读实现"对照 |
| 一堆 FatFs | `PlantOS\src\fs\fatfs\ff.c`(7114)、`CoolPotOS\module\fatfs\ff.c`(7167)、`Uinxed-Kernel\fs\fatfs\ff.c`(6699)、`Neumann\kernel\fs\fatfs\ff.c`(7108)、`xbook2\src\fs\fatfs\ff.c`(6846)、`MOOS\NativeLib\ff.c`(6204) | 6200~7200 | ChaN BSD-1（`XJ380\licenses\fatfs.txt` 原文） | **不建议**：前五个都是 `FF_USE_LFN=3`（动态分配 + `ff_memalloc` + Unicode 转换，MOOS 是 `=1` 静态缓冲版），要整套替换 `fatfs.c` + `fatfs_vfs.c`，等于把 17/17 的 FAT32 验收推倒重来 |
| XJ380 | `XJ380\driver\fs\fatfs\fatfs.cpp`(1524) + `include\fs\fatfs\ff.h` | 1524 | Apache-2.0 | C++ + 同样走 FatFs，不优 |

> 一句话结论：**「能直接搬代码」的是 Lnxrm（GPL-3.0）；「只能参考算法自己写」的是 SpiritFoxOS / HBOS / FatFs 系列**（前两个许可不清或实现有缺陷，FatFs 是架构不匹配，不是法律障碍）。

#### 2.2.3 Lnxrm 的关键算法（照抄这几处即可）

`Lnxrm\kernel\fs\fat32\fat32_dir.c`：

| 行 | 内容 | 要点 |
|----|------|------|
| 49-58 | `struct lfn_state { char longname[261]; int total, expect, name_end; u8 sum; }` | **261 = 20×13 + 1**（作者注释：旧代码 256 会让第 257~260 字符被静默丢弃，退化成 8.3） |
| 64 | `lfn_reset()` | 整块 `memset` 清零（只清三个计数器会残留上一链的字节，被下一短链读出来当名字） |
| 138-186 | LFN 项处理 | ① `seq = raw[0] & 0x1F`，② `marked = raw[0] & 0x40`（带 0x40 = 链首/最高序号），③ 非链首必须 `seq == expect` **且** `raw[13] == sum`（别名校验和），否则整链丢弃，④ 13 个 UCS-2 偏移表 `poff[13]={1,3,5,7,9,14,16,18,20,22,24,28,30}`，⑤ `0xFFFF/0x0000` 截断 |
| 197-202 | 采用条件 | `lf.total && lf.expect==0 && lf.name_end>0 && lf.sum == fat_lfn_checksum(e->name)` —— **四个条件缺一不可**，这是它比 SpiritFoxOS 严格的地方 |
| 5 | `fat_short_to_name(sn, ntres, out[13])` | NTRes（短名第 13 字节）的"全小写/基名小写/扩展小写"三态还原 |

映射到 SutureOS：`lfn_feed()` 已经照这个形状实现了（:361-382），但**建议核对三处**：`lfn_is_entry()` 是否检查 `ent[12]==0 && ent[26]==0 && ent[27]==0`（已做，:350）、`lfn_assemble()` 是否做了 `sum == lfn_checksum(sfn)`（:384 起，需复核）、以及 `readdir` 跨簇时 `lfn_acc` 是否随游标保存（:82 已挂在 `fatfs_dir_t` 上，正确）。

#### 2.2.4 LFN 写入（剩余工作）的算法来源

FatFs 的 `put_lfn()` + `gen_numname()`（任一 `ff.c`，如 `PlantOS\src\fs\fatfs\ff.c`）给出两段标准算法，抄算法不抄代码即可：

1. **生成别名**：取长名的合法字符 → 大写 → 基名截 8 / 后缀截 3 → 若与目录内已有项冲突，插 `~1`、`~2`…，冲突超过 `~999999` 报错。
2. **写链**：计算别名 11 字节的 checksum → 按 `seq = ceil(len/13)` 递减写 LFN 项（`0x40|seq` 只在最后/最高序号项置位）→ 最后写短名项。三个"空闲槽"必须**连续**（不连续就换下一段或用 `0xE5` 清零段）。

SutureOS 侧落点：`dir_free_slot()`(:524) 现在只找**一个**槽，需要扩展成"找 N+1 个连续槽"；`make_raw()`(:480) 改为"能压成 8.3 就用 8.3，否则走别名生成"。

#### 2.2.5 验收指标（可量化）

| # | 断言 | 做法 |
|---|------|------|
| 1 | 宿主生成的 LFN 链在 guest 能读 | `mkfat32_testimg.py` 造 `很长的中文文件名测试.txt`、`Readme.markdown`、`a.b.c.d.txt`；guest `ls /mnt/fat` 输出逐字符等于预期。（树内 `mkfat32_testimg.py` 已有 `lfn_checksum()`(:113) 与 `make_lfn_chain()`(:121) 两个夹具助手，但 `main()`(:358) 仍只写三个 8.3 文件——**把长名文件挂进 `build_image({...})` 是这一步的第一件事**） |
| 2 | `cat` 能按长名打开 | `cat /mnt/fat/很长的中文文件名测试.txt` → 内容逐字节等于镜像里的内容 |
| 3 | 长名目录能进 | `cd /mnt/fat/长名目录` + `pwd` 回显正确 |
| 4 | 链校验和生效 | 负例镜像：把某个 LFN 项的 `raw[13]` 改错 → guest `ls` 必须**回退显示 8.3 别名**，不得显示乱码、不得 PANIC |
| 5 | 序号断链 | 负例：删掉中间一项 → 同样回退 8.3 |
| 6 | 边界 | 255 字符（20 项全满）、13 的整数倍（260→截为 255）、跨扇区、跨簇各 1 例 |
| 7 | 删除不残留 | `rm` 长名文件后宿主解析镜像：其 LFN 链全部变 `0xE5` |
| 8 | **回归** | 宿主 `python scripts\verify_fat32_write.py build\fat32_dirty.img build\fat32.img` 仍 **17/17** |

宿主断言要在 `scripts/verify_fat32_write.py` 里新增 ≥5 项（现在 17 项），并且能在**不启动 QEMU**的情况下独立解析镜像验证 LFN 链结构（沿用该脚本已有的独立 FAT32 解析器）。

#### 2.2.6 工作量

**中**：改 1 个核心文件（`fatfs.c`，读侧已有人写了，写侧 +200~300 行）+ 2 个测试脚本（`mkfat32_testimg.py` 只需把已有 `make_lfn_chain()` 挂进 `main()`，`verify_fat32_write.py` 需新增 LFN 链结构断言）。风险点不是代码量，而是**不能破坏现有 17/17**：所有 8.3 路径必须原样工作（`make_raw()` 的 8.3 分支不要动）。

---

### 2.3 ★3 AC97 声卡（PCM16 播放）

#### 2.3.1 为什么选它

SutureOS 目前**完全没有任何音频通路**（`kernel/drivers/` 里没有 sound 相关文件）。AC97 是：

- 素材库里**最小的一份可用声卡驱动**（169 行，全文件读完就能懂）；
- QEMU 一条参数即可挂（`-device AC97`），VMware 也支持 AC97 兼容；
- 验收可以**完全量化**（宿主解析 QEMU 导出的 WAV 波形），不依赖人耳判断；
- 与 GUI 红线无冲突（内核侧播放，界面不加任何东西，最多 shell 加一条 `soundtest` 命令）。

#### 2.3.2 来源

`HBOS\src\ac97.c`(169) + `HBOS\src\ac97.h`，许可证 **GPL-3.0**（`HBOS\LICENSE` 实读为 GNU GPL v3）。

| 行 | 函数 | 要点 |
|----|------|------|
| 68 | `ac97_init()` | `pci_find_class(0x04,0x01)`（多媒体/音频）→ **CMD 置 bit0(I/O Space) + bit2(Bus Master)**（注释明确：不能复用"只置 Memory Space"的辅助函数）→ BAR0=NAM（mixer，I/O）、BAR1=NABM（bus master，I/O）→ codec 冷复位 + 主音量/PCM 音量写 0（0dB）→ PCM OUT `CR.RR` 复位自清等待 → 分配 32 条 BDL |
| 117 | `ac97_play_pcm16(samples, frames, channels)` | 总采样数上限 `32 × 0xFFFE`（≈44 秒立体声）→ 填 BDL（每项 buffer_ptr 物理地址 + num_samples + flags=0）→ `BDBAR` / `LVI=last` / `CR=RPBM` → 轮询 `PO_SR.DCH` 等播完 → `CR=0` 停 |
| 24-34 | NABM 偏移 | `PO_BDBAR 0x10 / CIV 0x14 / LVI 0x15 / SR 0x16 / PICB 0x18 / CR 0x1B`；`SR.DCH=0x0001`、`CR.RPBM=0x01`、`CR.RR=0x02` |
| 37-39 | NAM 偏移 | `RESET 0x00 / MASTER_VOL 0x02 / PCM_VOL 0x18` |

#### 2.3.3 依赖替换（这里是全部工作量所在）

| HBOS 依赖 | SutureOS 对应 | 适配动作 |
|-----------|----------------|----------|
| `pci_find_class(0x04,0x01,0xFF,&dev)` | **没有通用 PCI 类扫描器** | 照 `kernel/drivers/rtl8139.c:182` 的 `pci_cfg_read32()` 写 ~40 行：遍历 bus/dev/fn，读 offset 0x08 的 class code 低 24 位 == 0x040100 |
| `kmalloc_aligned(…, 8)` + `vmm_virt_to_phys()` | `moe_pmm_alloc(1)` 或 `.bss` | **用 static `.bss`**（`ac97_bdl_entry_t g_bdl[32] __attribute__((aligned(8)))`），恒等映射 ⇒ 物理地址==`(uint32_t)(uintptr_t)&g_bdl`，零转换（同 `rtl8139.c:118` 注释） |
| `hw_deadline_start/expired_ms` | `g_tick_count` / `mbos_pit_counter()` | 照 `rtl8139.c` 的"有界自旋 + hlt 单独限次"写法 |
| `task_yield()`（:159 等播完用） | **必须删掉！** | SutureOS 红线：GUI/shell 主循环禁 `sched_yield`（task.c 轮转与 EEVDF 共用 context，会跳到陈旧上下文并刷 `vector=6 #UD`）。改成有界 `pause` 自旋 + 超时（同 `gui_pace()` 的做法） |
| `io_out8/16/32`、`io_in8/16` | `include/stitch/io.h` 的 `outb/outw/outl/inb/inw` | 改名即可，语义一致 |
| 采样率 | 固定 48000Hz（HBOS 明确不做 VRA/重采样） | **沿用**：播放前把 WAV/正弦表的采样率固定成 48k，别去实现重采样 |

#### 2.3.4 移植步骤草案

1. `kernel/drivers/ac97.h`：`ac97_init()/ac97_present()/ac97_play_pcm16(const int16_t*, uint32_t, int)` + 错误码，注释写来源与 GPL-3.0。
2. `kernel/drivers/ac97.c`：按上面 3 条替换（static BDL、无 `task_yield`、自写 PCI class 扫描）。
3. `kernel/main/kmain.c`：`ac97_init()` 放 NIC 之后（都不影响启动），失败只打 `[ac97] not found`，**不得 PANIC**。
4. `kernel/shell/shell.c`：加 `soundtest`（内置 440Hz 正弦 1 秒 + 可选 `soundtest <hz> <ms>`），走 `ac97_play_pcm16()`。
5. `kernel/main/selftest.c`：`st_ac97_test()` —— 无卡时记 "skipped"（同 `st_rtl8139_test()` 的宽容策略）。

#### 2.3.5 验收指标（可量化，全自动）

| 环境 | 命令 | 判据 |
|------|------|------|
| QEMU + 录 WAV | `-audiodev wav,id=a,path=build\audio.wav -device AC97,audiodev=a` → shell `soundtest 440 1000` | 宿主 Python（`wave` 模块，零依赖）断言：① 时长 **1.00 ± 0.05 s**；② 峰值 ≥ **0.10 FS**（不是静音）；③ 过零率换算频率 **440 ± 22 Hz**（±5%）；④ 前 50ms 与后 50ms 无削波（`|x| < 0.99 FS`） |
| QEMU 无声卡（负例） | 不加 `-device AC97` | 串口 `[ac97] not found`；`soundtest` 返回负值并打一行提示；**不得 PANIC、不得卡死** |
| 启动回归 | 完整启动日志 | 自检行数不变（13/14 之类原有判定不退步）；`[term] selftest: 25 px visible` 仍在；`startgui` 后 GUI 外观与基线截图逐项一致（红线验收） |
| VMware | `vmware_boot.ps1` | 出现 `[ac97] codec ready` 或 `not found`（二者都是合法结果，VMware 音频后端不保证 AC97） |

#### 2.3.6 工作量

**小**：2 个新文件（≈250 行，含自写 PCI class 扫描）+ `kmain.c`、`shell.c`、`selftest.c` 各 1 处 = **5 个文件**。

> 若要求更好的兼容性，备选是 `VimtuOS\kernel\hda64.cpp`(1107, GPL-3.0) 的 Intel HDA（需 codec 节点遍历 + 命令/响应环 + 流描述符，工作量大一个档）或 `PlantOS\src\kernel\drivers\sound\hda.c`(695, MIT)。**建议先用 AC97 打通"系统能出声"这条链路，HDA 留作后续。**

---

## 3. 其它候选速评（各一行）

| 候选 | 本地来源 / 许可证 | 判断 |
|------|-------------------|------|
| **NVMe FLUSH** | `Cinux\kernel\driver\nvme\nvme.cpp:328` `flush()`；MIT | **值得做**：SutureOS `kernel/drivers/nvme.c`(928) 没有 flush；只需 admin 命令 `0x01 (FLUSH)` + 在 `blk_*` 的 sync 路径调用，30~60 行。验收：`sync` 后宿主镜像 CRC 与写后一致；用 QEMU `-device nvme` 抓 `AQA/ASQ` 日志确认命令完成 |
| **virtio-blk flush** | `Uinxed-Kernel`/`Cinux` 的 virtio 均可参考 | 同上，VIRTIO_BLK_T_FLUSH(4)，~20 行；SutureOS `virtio_blk.c`(714) 目前无 flush |
| **ATAPI / SATA CD-ROM** | `Uinxed-Kernel\drivers\block\ata\pata\atapi.c`(441, Apache-2.0)、`XJ380\driver\ahci\atapi.cpp`(93)、`CoolPotOS\src\driver\ahci\atapi.c`(86)、`LunaixOS\lunaix-os\hal\ahci\atapi.c`(91) | 值得做（中）：SutureOS `ahci.c` **只识别不驱动**——`ahci.c:238-239` 定义了 `AHCI_SIG_ATAPI=0xEB140101`，`:962-963` 命中后只打 `[ahci] port N: ATAPI device (sig=0x..), not supported by this driver`。**注意顺序依赖**：想 `cat` 光盘文件，ATAPI 与 ISO9660 必须一起做 |
| **ISO9660** | `CoolPotOS\module\iso9660\iso9660.c`(432, MIT)、`PlantOS\src\fs\iso9660.c`(569, MIT)、`Uinxed-Kernel\fs\isofs\isofs_vfs.c`(683, Apache-2.0)、`Neumann\kernel\fs\iso9660.c`(412, GPL-3.0) | 值得做（中）：**可以先做不依赖 ATAPI 的形态**——把 ISO 镜像挂成 raw 块设备（`ata1`/`ram0`）直接挂 ISO9660，宿主验收立刻可跑；ATAPI 之后再补 |
| **ext4 / ext2 写** | `XJ380\driver\fs\ext4\ext4.cpp`(1762, Apache-2.0, C++)、`Deshab-OS\CODE\dsk\ext4.c`(737, Apache-2.0, C)、`MoQiOS\kernel\fs\ext4.zig`(616, GPL-3.0, Zig) | 不建议本轮（大）：SutureOS `ext2.c` 只读 1078 行；ext4 写要 extent 树 + 日志 + 校验和，等于新写一个文件系统。真要动手，`Deshab` 的 737 行 C 是最省力的起点 |
| **USB EHCI** | `Uinxed-Kernel\drivers\usb\host\ehci\`(1 文件 890, Apache-2.0)、`Deshab-OS\CODE\DKM\ehci\`(1 文件 1169) | 不建议（大）：EHCI 只给 USB2；要可用还需 hub 枚举 + HID boot protocol 解析；且 SutureOS 键鼠现状是"轮询 PS/2"并已踩过 IRQ 丢字节的坑，USB 只能更差。真要做优先 xHCI 而不是 EHCI |
| **USB xHCI** | `XJ380\kmod\xhci`(5 文件 5058, Apache-2.0)、`Uinxed-Kernel\...\xhci`(2 文件 1712)、`CoolPotOS\src\driver\usb\xhci`(15 文件 1407, MIT) | 不建议（大）：全是"驱动 + 类驱动 + HID"套装，剥离成本 ≥ 一个子系统 |
| **HDA 声卡** | `VimtuOS\kernel\hda64.cpp`(1107, GPL-3.0)、`PlantOS\src\kernel\drivers\sound\hda.c`(695, MIT) | 与 ★3 二选一：HDA 兼容更好但工作量大一个档；先 AC97 打通链路 |
| **virtio-gpu 2D** | `Deshab-OS\CODE\UTSM\vmm\virtio_gpu.c`(984, Apache-2.0)、`VimtuOS\kernel\virtio_gpu64.cpp`(1552, GPL-3.0)、`Uinxed-Kernel\drivers\gpu\drm\virtio\*.c`(DRM 套装) | 收益低：SutureOS 已有 framebuffer + `gui.c` 直接画；virtio-gpu 只换后端，用户看不到差别。若为了 QEMU 下"无 framebuffer 也能显示"，性价比不高 |
| **图片查看器** | 解码：`HBOS\src\bmp.c`(67, GPL-3.0)、`Uinxed-Kernel\libs\gfx\bmp.c`(55, Apache-2.0)、`PlantOS\src\plic\qoi.c`(516, MIT)、`MOOS\NativeLib\lodepng.c`(6647, zlib) | 可做（小-中）：BMP 最省（67 行，无压缩）；QOI 次之（516 行，MIT）；PNG 要么 6647 行 lodepng（zlib 许可可融合）要么自己写 inflate（SutureOS 已有 `kernel/pkg/inflate.c` **270** 行可复用！）。**UI 必须自绘** |
| **HTTP 下载客户端** | ring-3：`HBOS\src\user\progs\wget.c`(124, GPL-3.0)、`Deshab-OS\CODE\tools\curl\main.c`(418, Apache-2.0)、`Mini-OS\v2-c-kernel\src\app\httpdemo.c`(100, MIT)、`VimtuOS\user\browser\net.c`(882, GPL-3.0) | 可做（小）：SutureOS **socket 系统调用已注册**（`syscalls2.c:1235` `reg(SYS_SOCKET, …)`，41~55 全套），TCP/DNS/DHCP 都在；缺的只是一个 ring-3 程序。最小 124 行即可跑通 `GET` |
| **ring-3 文件工具** | 自写；`VimtuOS\user\busybox\vimtu_dirent.c`(351, GPL-3.0) 作参考 | 可做（中）：内核 shell 已有 `ls/cat/cp/mv/rm/mkdir/rmdir/touch`（`kernel/shell/shell.c`），**缺的是 `user/apps/` 下的 ring-3 版本**（现在只有 `hello/probe/shell/forktest/sigtest`）。`getdents64/stat/open/read` 都已就绪 |
| **文本编辑器** | `VimtuOS\user\apps\edit\edit.c`(604, GPL-3.0) | 谨慎：功能可借（键位、行缓冲、保存逻辑），**界面一律用 `kernel/gui/apps.c` 的 `gui_fill_rect/gui_draw_str` 自己画**（红线第 ② 条）。若做成终端行编辑器（无窗口）则风险最低 |
| **包管理器** | 已有 `kernel/pkg/deb.c` + `inflate.c`；参考 `VimtuOS\user\dpkg\dpkg.c`(470, GPL-3.0) | 低优先：`.deb` 安装内核侧已能跑（`[deb] installed hello 1.0-1`），缺的只是 CLI 与依赖解析 |
| **ELF 加载器** | **不需要移植**：`kernel\syscall\loader.c`(123) 已在，启动路径没用它 | 做"接线"不做"移植"：把 `loader.c` 接到 `execve`（当前走的是扁平 blob 路径），验收 = 把一个静态链接的 musl 程序丢进 FAT32 卷，`run foo` 能跑起来。`VimtuOS\kernel\elf64.cpp`(1331, GPL-3.0) 只在需要 PT_DYNAMIC/重定位时才参考 |
| **计算器 / Task Manager / About** | 已有 `kernel\gui\apps.c`(269) | 不做：SutureOS 已有三应用（Task Manager / Calculator / About），再多也只是数量变多 |
| **窗口管理器增强（拖动/缩放/剪贴板/吸附）** | 只能借功能：`XJ380`、`VimtuOS\kernel\explorer64.cpp`(3079)、`startmenu64.cpp`(1888)、`gui64.cpp`(3497) | **不做**（红线 ④：不得替换/搬入别人的桌面环境）。拖动已在 v0.11.5 修好；要加剪贴板就自己写 |

---

## 4. 许可证风险清单

### 4.1 禁止融合（GPL-2.0-only，与 SutureOS 的 GPL-3.0 不兼容）

GPL-2.0 与 GPL-3.0 是**双向不兼容**的许可，把 GPL-2.0-only 代码并入 GPL-3.0 内核是非法的（不是"注明出处就行"）。以下仓库**只能读算法、重写实现**，不能拷代码：

| 仓库 | 本地 LICENSE 实读 | 为什么提 | 替代 |
|------|-------------------|----------|------|
| DragonOS | GPL v2, June 1991 | 有 e1000e(Rust) 驱动、ext4、网络栈 | 用 MIT/Apache 的同类实现 |
| Utopia / Utopia-2024 | GPL v2 | Rust 内核 | — |
| Reedx-Kernel | GPL v2 | 1.4 万行 x86_64 宏内核、FAT32 | Lnxrm/PlantOS 的 FAT32 |
| hudos | **无 LICENSE 文件**，README 首行声明 GPLv2 | 裸机 MMU/syscall 示例 | 视同 GPL-2 处理（且许可自述不充分） |

### 4.2 AGPL-3.0（网络服务条款，比 GPL-3 更严）

| 仓库 | 实读 | 结论 |
|------|------|------|
| UniOS | `AGPL v3` | **不可融合**（AGPL 与 GPL-3 虽可单向兼容，但 AGPL 的 §13 网络条款会传染整个系统；SutureOS 是要发布的系统，风险不值得） |

### 4.3 无 LICENSE 文件 → 只能参考算法 / 需联系作者

| 仓库 | 本地核实结果 | 影响 |
|------|--------------|------|
| **SpiritFoxOS** | 根目录无 LICENSE/COPYING/NOTICE；全仓库 `*.md`/`*.txt` 里**没有任何 "GPL" 字样**；README 也没有许可段 | **最需要处理的一条**：SutureOS 现有 `kernel/net/net.h`、`kernel/net/net.c`、`kernel/drivers/rtl8139.c`+`.h`、`kernel/fs/fatfs.c`+`.h` 的头注释都写着 "Ported from SpiritFoxOS … GPL-3.0"。既然上游没有许可声明，这个出处依据不足。建议：① 头注释改成"上游未声明许可证，仅作结构与算法参考，实现为本项目独立编写"；或 ② 发信给作者确认 GPL-3.0 |
| C-UnknownOS | 无 LICENSE | 不要用（任务里列它是为了 LFN，但见 5.4） |
| QNOS | 无 LICENSE | 不要用 |
| KunOS | 无 LICENSE（且是纯汇编 16 位） | 不要用 |
| GrapeOS-Course | 无 LICENSE（配套课程代码） | 不要用（GrapeOS 本体是 Mulan PSL v2，可融合，但只是 4000 行 32 位系统） |
| aether-naos | 无 LICENSE | 不要用（编排仓，无内核代码） |

### 4.4 与清单/树内注释不符的事实（需要更正的三处）

1. **Nilix 不是"整个 AGPL"**：本地无 `LICENSE` 文件，用的是 REUSE 多许可模型（`LICENSE-AGPL`/`LICENSE-APACHE`/`LICENSE-GPL`/`LICENSE-CC-BY` + `REUSE.toml` + `COPYRIGHT`）。按 `COPYRIGHT` 的映射：**内核/bootloader/绝大多数 Rust 代码 = Apache-2.0**；AGPL-3.0 只覆盖 `scripts/ fuzz/ tools/ kernel/tests/ userspace/*fuzz* .github/`；GPL-3.0 覆盖 `**/*.c` 与 Makefile；文档图片 CC-BY-4.0。所以"Nilix 是 AGPL 不能碰"这个说法对内核部分不成立——但它是 Rust 多架构内核，对 SutureOS 无实操价值。
2. **树内 `kernel/drivers/e1000.h:1-2` 的许可证标注是错的**：它写 "ported from Cinux' … e1000.cpp, **GPL-3.0**, same project family"，而 `Cinux\LICENSE` 实读是 **MIT License, Copyright (c) 2026 Charliechen114514**。MIT → GPL-3.0 完全合法，但出处许可写错在发布物里是硬伤，建议更正为 MIT。
3. **ReliefOS 本地没有内核源码**：`LICENSE` 是 Apache-2.0，但 `kernel/` 只有一个空目录 `kernel/reliefnt/`（`.gitmodules` 指向 `ReliefOSProject/ReliefNT` 子模块，未检出）。所以它那份 153MB 里只有 `tools/tests/`（含 `e1000_init_test.c`、`ext4_*_test.c`）和 `userland/`，**不能作为任何驱动/文件系统的代码来源**——只能当"测试用例写法"参考。

### 4.5 可融合但要注意的具体条款

| 来源 | 许可 | 融合到 GPL-3.0 | 注意 |
|------|------|----------------|------|
| Cinux / CoolPotOS / Mini-OS / PlantOS / HanOS / LunaixOS / MOOS / COFUOS / MikuOS / SINO-OS / MCubeAI-OS / VergeOS-Open / ViperOS / xbook2 | MIT（MOOS 为 Unlicense） | ✅ | 保留版权声明与许可全文 |
| XJ380 / Uinxed-Kernel / Deshab-OS / LianKernel / Rootware | Apache-2.0 | ✅（GPL-3.0 明确接受 Apache-2.0） | 保留 NOTICE；注意 Apache-2.0 的专利条款 |
| Lnxrm / HBOS / Neumann / Powerint-DOS-386 / MoeOS / Lemis / Phobos / na-kernel / VimtuOS / MoQiOS / SutureOS | GPL-3.0 | ✅ | 同许可，最省事；保留作者与出处 |
| MandelbrotOS | MPL-2.0 | ✅（文件级 copyleft，可并入） | 被修改的 MPL 文件需保持 MPL |
| NAOS | BSD-3-Clause | ✅ | 保留版权与免责 |
| GrapeOS | Mulan PSL v2 | ✅ | — |
| **FatFs 全系列**（各仓库的 `ff.c`） | **ChaN 自有 BSD-1 条款**（`XJ380\licenses\fatfs.txt` 原文：保留版权声明 + 免责） | ✅ | 它**不是** MIT；融合时按原文保留 "Copyright (C) 2022, ChaN" 与免责声明 |
| **busybox**（`VimtuOS\third_party\busybox-1.36.1`、`NAOS\...\busybox`、ReliefOS 子模块） | **GPL-2.0** | ❌ **不能编进内核** | GPL-2-only 与 GPL-3 不兼容；`objcopy -I binary` 把 busybox 嵌进内核 = 链接 = 派生作品。作为独立 ring-3 ELF 放在 FAT32 卷上属"聚合"，法律上可辩但仍灰色 → **建议自写工具，busybox 只当行为规范** |

### 4.6 语言层面直接出局（不是许可问题）

`MoQiOS`（Zig）、`ViperOS`（自研 .vp 语言）、`Nilix`/`Utopia`/`LianKernel`/`Rootware`（Rust）、`MOOS`（C#/.NET AOT）——SutureOS 是纯 C + 汇编 + 少量自研用户态 libc，搬这些等于重写。

---

## 5. 「看起来很好但实际不可移植」清单

| # | 看起来很好 | 为什么不可移植 |
|---|-----------|----------------|
| 5.1 | **ReliefOS** 的 e1000 / ext4 / AC97 / ALSA / busybox 全家桶（Apache-2.0，153MB，"原 LeonOS 4，自研内核 + Linux ABI 兼容层"） | 本地 `kernel/` 是**空子模块**（`kernel/reliefnt` → `ReliefOSProject/ReliefNT` 未检出）。仓库里只有 `tools/tests/*_test.c`（宿主测试）与 `userland/`。**没有任何驱动实现可读**。它被任务书列为 e1000 候选，实际不成立。 |
| 5.2 | **xbook2** 的 e1000（MIT，`e1000.c` 1772 + `e1000_hw.c` **5480** 行） | 这是 32 位时代把 Intel 官方 `e1000_hw.c`（寄存器/EEPROM/PHY 全套抽象，BSD 衍生）搬进内核的产物。体量等于"重写一个 e1000"，且大量 32 位假设（物理地址截断、`u32` 指针强转）。MIT 许可不是问题，**工作量**才是。 |
| 5.3 | **DragonOS** 的 e1000e（Rust，1225★，工程最完整） | 双重否决：**GPL-2.0** 与 GPL-3.0 不兼容 + Rust 语言不通。 |
| 5.4 | 任务书里说"**Mini-OS**（MIT）有 FAT32 LFN"、"**HanOS**（MIT）有 FAT32" | **都不成立**：`Mini-OS\v2-c-kernel\src\fs\`（`fs.c` 605 / `storage.c` 344）里没有任何 LFN 相关代码（grep `LFN|0x0F|long name|长文件名` 零命中）；`HanOS\kernel\srv\fat32_srv.c`(443) 同样零命中，且它把 FAT32 放在一个 ring-3 server 里，接口模型与 SutureOS 的 in-kernel VFS 完全不同。 |
| 5.5 | **CoolPotOS** `module/e1000/e1000.c`（MIT，"社区 star 最高"） | **10 行**，是个桩/占位文件，不是驱动。CoolPotOS 里真正可用的是 `module/fatfs/ff.c`（FatFs）与 `module/iso9660`。 |
| 5.6 | **MCubeAI-OS**（MIT，`kernel/net/e1000.c` 282 行）、**SINO-OS**（MIT，9.5MB） | 作者自述用 TRAE/Codex + GPT 生成，提交数 11 / 4，没有可验证的运行证据。当"AI 辅助写 OS 的样本"可以，当移植来源不行（无法判断哪些是被验证过的实现）。 |
| 5.7 | **FatFs 整套替换**（`ff.c` 6200~7200 行，MIT/BSD-1 都有） | 许可没问题，**架构不匹配**：`FF_USE_LFN=3` 需要 `ff_memalloc` 动态分配 + 磁盘 I/O 回调 + 一整套 `f_open/f_readdir` API，会取代现有 `kernel/fs/fatfs.c`(1273) 与 `fatfs_vfs.c`(276)，把 17/17 的 FAT32 写验收推倒。**只该抄它的 `put_lfn()/gen_numname()` 算法**。 |
| 5.8 | **MOOS** `NativeLib/lodepng.c`(6647) 做 PNG | 许可（zlib）没问题，但 PNG 解码在 GUI 里只是"能看一张图"；而 SutureOS 已有 `kernel/pkg/inflate.c`(270) 可复用自写最小 PNG 解码（非隔行），比搬 6647 行更干净。 |
| 5.9 | 各家**桌面/窗口管理器**（`VimtuOS\kernel\explorer64.cpp` 3079、`startmenu64.cpp` 1888、`gui64.cpp` 3497；`XJ380` 桌面） | **红线直接禁止**（用户 2026-10-23：不得用移植名义替换现有 GUI 框架/合成器/窗口管理器，不得把别人的桌面环境整层搬进来；v0.12–v0.14 就因此回滚过）。只能读它们的"功能清单"，界面自己画。 |
| 5.10 | **busybox / dpkg / bash 类"整装用户态"** | ① **许可**：busybox 是 GPL-2.0，不能编进 GPL-3 内核；② 就算自写，也要先确认 fork/exec 的成熟度——虽然 v0.11.4 已把 `fork(COW)+wait4+getppid` 做出来（`process.c`、`reg(SYS_WAIT4,…)` 在 `syscalls2.c:1192`），但 shell 管道/作业控制尚未验证。**结论：工具自己写，busybox 只当行为规范。** |
| 5.11 | **ext4 读写**（XJ380 1762 行 C++ / Deshab 737 行 C / ReliefOS 一堆 `ext4_*_test.c`） | ReliefOS 只有测试没有实现（5.1）；XJ380 是 C++ 且绑它自己的 VFS；Deshab 737 行虽是 C 但只是 ext4 的**子集**（无 extent 树深度、无日志重放）。SutureOS 当前 `ext2.c` 只读，要写就得自己写 extent + 日志，**不是"搬一个文件"的量级**。 |
| 5.12 | **USB 键鼠**（"补上 USB 就不用 PS/2 了"） | SutureOS 的 8042 轮询刚在 v0.6.2 修好（aux 字节必须被消费否则缓冲永久占满）。USB 引入 EHCI/xHCI + hub 枚举 + HID 报告解析 + 端口复位时序，**风险远大于收益**；而且 QEMU/VMware 下 PS/2 键鼠已经够用。 |
| 5.13 | 把 **Cinux e1000 的 C++ 类**直接塞进内核 | 会引入 C++ 语义（构造/命名空间/`bool`/引用）而 SutureOS 无 C++ 运行时（无 `new/delete`、无 `__cxa_*`）。必须转成 C（或只抄算法）。 |

---

## 附录 A：素材库快速索引（本报告引用的路径）

```
自研操作系统\
  VimtuOS\kernel\e1000_64.cpp              507  GPL-3.0   ★1 首选
  VimtuOS\kernel\e1000_64.h                 16
  Cinux\kernel\driver\net\e1000.*          616  MIT       备选（C++）+ 3 篇踩坑笔记
  Cinux\document\notes\2026-06-26-f5-m6-e1000-rx-timer-fix.md   ← RDH 轮询陷阱
  na-kernel\modules\drivers\net\e1000\e1000.c  556  GPL-3.0
  Mini-OS\v2-c-kernel\src\drv\e1000.c      608  MIT
  XJ380\kmod\e1000\e1000.cpp               494  Apache-2.0
  Lnxrm\kernel\fs\fat32\fat32_dir.c        368  GPL-3.0   ★2 首选
  SpiritFoxOS\recovered\fs\src\fat32.c    1528  无 LICENSE
  HBOS\src\fat32.c                         911  GPL-3.0
  PlantOS\src\fs\fatfs\ff.c               7114  MIT 仓库 / ChaN BSD-1 文件
  HBOS\src\ac97.c                          169  GPL-3.0   ★3 首选
  PlantOS\src\kernel\drivers\sound\hda.c    695  MIT
  VimtuOS\kernel\hda64.cpp                1107  GPL-3.0
  Cinux\kernel\driver\nvme\nvme.cpp        365  MIT        flush() @328
  Uinxed-Kernel\drivers\block\ata\pata\atapi.c 441 Apache-2.0
  CoolPotOS\module\iso9660\iso9660.c       432  MIT
  Deshab-OS\CODE\UTSM\vmm\virtio_gpu.c     984  Apache-2.0
  HBOS\src\bmp.c                            67  GPL-3.0
  VimtuOS\user\apps\edit\edit.c            604  GPL-3.0
  VimtuOS\user\dpkg\dpkg.c                 470  GPL-3.0
  HBOS\src\user\progs\wget.c               124  GPL-3.0
  XJ380\licenses\fatfs.txt                        ← ChaN 许可原文
  Nilix\REUSE.toml + Nilix\COPYRIGHT              ← 多许可映射（内核 Apache-2.0）
  ReliefOS\.gitmodules                            ← kernel/ 是未检出子模块
```

## 附录 B：三条 ★ 的最小验证命令

```powershell
# ★1 e1000（QEMU，能 ping 通 = 网络通）
& 'C:\Program Files\qemu\qemu-system-x86_64.exe' -machine pc -m 256 -display none `
  -serial file:build\serial-e1000.log -cdrom build\sutureos.iso -boot d `
  -hda build\fat32.img -smp 2 -netdev user,id=n0 -device e1000,netdev=n0 `
  -monitor stdio -no-reboot
# 然后在 guest shell: ping 10.0.2.2   → 期待 3/3；宿主 grep serial-e1000.log 里的 [e1000] 行

# ★1 VMware（用户真实环境，这才是收益所在）
powershell -NoProfile -File scripts\vmware_boot.ps1 -SerialLog build\vmware_serial.log -WaitSeconds 60
# 期待串口出现 [e1000] mac= 与 [net] selftest ping 10.0.2.2 -> reply

# ★2 LFN（不用 QEMU 也能验一半：宿主独立解析镜像）
py -3 scripts\mkfat32_testimg.py                     # 无参数脚本；要把长名文件挂进 main() 的 build_image({...})
py -3 scripts\verify_fat32_write.py build\fat32_dirty.img build\fat32.img   # 期待 17/17 不退步
# guest 侧: ls /mnt/fat  → 长名完整显示；cat /mnt/fat/<长名> → 内容正确

# ★3 AC97（播放结果落在宿主的一个 wav 里，可以逐样本断言）
& 'C:\Program Files\qemu\qemu-system-x86_64.exe' -machine pc -m 256 -display none `
  -serial file:build\serial-ac97.log -cdrom build\sutureos.iso -boot d `
  -hda build\fat32.img -audiodev wav,id=a,path=build\audio.wav -device AC97,audiodev=a
# guest shell: soundtest 440 1000
# 宿主 py 断言: 时长 1.00±0.05s / 峰值 ≥0.10FS / 过零率 440±22Hz
```

---

## 一句话总结

素材库里**真正能直接搬、且值得搬**的只有三件：**VimtuOS 的 e1000（GPL-3.0，小）**、**Lnxrm 的 LFN 重建算法（GPL-3.0，中）**、**HBOS 的 AC97（GPL-3.0，小）**；许可证地雷集中在 **GPL-2.0 的 DragonOS/Utopia/Reedx/hudos（绝对不能用）**、**无 LICENSE 的 SpiritFoxOS（而 SutureOS 现有三处代码标着源自它，需要更正表述）**、**GPL-2.0 的 busybox（不能编进内核）**，以及两处"清单说有、本地其实没有"的坑（**ReliefOS 内核是空子模块**、**Mini-OS/HanOS 并没有 LFN**）。
