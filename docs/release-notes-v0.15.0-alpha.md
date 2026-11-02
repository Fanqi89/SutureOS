## 🚀 本次更新 (v0.11.5 → v0.15.0)

**两块从素材库移植进来的真实能力：Intel e1000 网卡（VMware 里终于有网了）与
FAT32 VFAT 长文件名读取（`ls` / `open` / `stat` 都认长名字）。启动方式与 GUI 外观未变。**

> 版本号说明：`v0.12.0/0.13.0/0.14.0-alpha` 三个 tag 指向上一轮被判"外观被改动"而
> **没有合入 main** 的 `wip/all-ports` 提交，按发布纪律旧 tag 不移动不删除，因此本次从
> v0.11.5 继续，版本号跳到 **v0.15.0-alpha**，与旧 tag 共存不冲突。

---

### 📦 移植 1：Intel e1000 (8254x) 网卡驱动

| 项目 | 内容 |
|---|---|
| 上游 | **Cinux** `kernel/driver/net/e1000.cpp`（MIT，Copyright (c) 2026 Charliechen114514） |
| 落点 | `kernel/drivers/e1000.c` / `e1000.h`（354 行）、`kernel/net/net.c` 增加链路层分发 |
| 为什么 | VMware Workstation 只提供 e1000 虚拟网卡（没有 RTL8139），此前 VMware 下**完全没有网络** |

- **链路层抽象**：`kernel/net/net.c` 新增 `ll_send()` / `ll_poll_rx()`，IP 栈不再直连
  RTL8139；两块网卡可互换。
- **选择顺序**：RTL8139 优先（它的轮询收包在中断屏蔽的开机自检里也能work，启动日志因此
  保持一条真实的 PASS），e1000 作为回退——**VMware 上就只有 e1000**。
- 保留上游三个踩坑结论：轮询收包必须先做一次 MMIO 读（QEMU 设备主循环才会推进）、
  `RDT = N-1` 哨兵、`RCTL = EN|SECRC|BAM|UPE`；TX 描述符需要 `EOP|IFCS|RS` 且门铃写 `TDT`。
- SutureOS 侧结构差异：内核是平坦恒等映射，DMA 缓冲直接用 `.bss`（不需要 DMA 池）。

### 📦 移植 2：FAT32 VFAT 长文件名（读侧）

| 项目 | 内容 |
|---|---|
| 上游 | **PlantOS**（MIT）ChaN FatFs 血统 + **SpiritFoxOS** 的长名清理逻辑（该仓库无 LICENSE，见下） |
| 落点 | `kernel/fs/fatfs.c`（+243 行） |
| 为什么 | 此前 FAT32 只认 8.3 短名，`ls` 看不到真实文件名，超过 8.3 的名字直接被拒绝 |

- `readdir` 会把一条 LFN 链拼回显示名（`ls` 因此显示 `Long file name test.txt`）。
- **本次补齐了 wip 分支没有的那一半**：wip 里只做了"显示"，`open/stat` 仍按 8.3 解析，
  长名文件打不开。现在 `path_resolve()` 逐段先用 8.3、失败再查 LFN 链，取到**短名别名**
  后交给原有 8.3 机制，于是 `open`/`stat`/`cat`/VFS 全部能用长名打开。
- 校验和（短名 11 字节）不匹配的链会被丢弃，避免残留条目被误读；删除/改名时把链标记为
  已删除，防止孤儿。
- **写侧仍是 8.3**（创建/改名不生成 LFN），已在限制里写明。

---

### ✅ 验收（真实 QEMU / 真机脚本，不是口头结论）

| 项目 | 结果 |
|---|---|
| 新脚本 `scripts/nic_acceptance.py` | **两块网卡各自 PASS**：link-up 行 + 交互 `ping 10.0.2.2` **3/3 回复** |
| RTL8139 回归 | 开机 `[net] selftest ping 10.0.2.2 -> reply in 1178 us`、`frames rx=2 tx=2 arp_entries=1` |
| e1000 | `[net] e1000 found at 0xfebc0000, mac=52:54:00:12:34:56`；交互 ping 3/3（1990/25/25 µs） |
| FAT32 长名自检（新增） | `[fat32] lfn self-test PASS (open=1 payload=1 readdir=1)` |
| FAT32 写/目录操作 | 干净镜像 `[fat32] ops self-test PASS (mkdir/rmdir, rename, unlink: 3/3)` |
| 宿主侧独立校验 | `py -3 scripts/verify_fat32_write.py build/fat32_ata.img build/fat32.img` → **checks=17 ok=17 fail=0** |
| 模块自检 | `[stitch] selftests: 13/14 passed`（`moos_intr` 为既有长期未修项，与本版无关） |
| 系统调用 | `syscalls: 81 registered` |
| 启动日志 | 266 行（v0.11.5 同基线；LFN 平台 +1 行，仅 e1000 平台多 4 行驱动/诊断行） |
| GUI 外观 | 新脚本 `scripts/gui_appearance_check.py`：与 `docs/baseline/base_gui.png` 对比，**动态矩形外差异 0 像素**（矩形内 377 px 全是 PMM/uptime/FPS/CPU 占用/版本号） |
| 无 PANIC | 全程无 PANIC |

### 🧪 本版新增的自动化脚本

- `scripts/nic_acceptance.py`：分别用 `-device rtl8139` 与 `-device e1000` 启动 ISO，
  等 shell 提示符后**用 QMP 注入 `ping 10.0.2.2`**，断言链路识别行与 ≥3 条回复。
- `scripts/gui_appearance_check.py`：外观门禁（只容忍文档化的动态数值矩形）。

### ⚠️ 已知限制（如实记录，不掩盖）

1. **e1000 的开机自检 ping 在 QEMU 11.1 下超时**：`GPRC=0`（设备模型没有把收到的包投递进
   ring），而**同一次启动里交互式 `ping` 3/3 正常**——驱动收发是好的，是"中断屏蔽 + 轮询 +
   让出 vCPU"这段自检环境和设备模型的配合问题；wip 分支上一轮的实现同样复现，
   所以不是本次引入的回归。`scripts/nic_acceptance.py` 用交互路径做权威验收。
   日志里会打印 `[net] selftest ping TIMEOUT (... yields=N)` + 一行 `[net] e1000 diag:`，
   再跑一次 `ping` 即可看到 3/3。
2. **e1000 是轮询驱动**（无中断、无 IMS），和 RTL8139 一样靠 `net_poll()` 抽包。
3. **FAT32 长名只支持读**；创建/改名仍产出 8.3 名字。LFN 解析上限 255 个 UTF-16 单元 / 20 个条目。
4. e1000 在 VMware 上只验证到"识别 + 链路 up + 收发"层面；VMware NAT 网段与 slirp 不同，
   要真正上网仍需先 `dhcp`（DHCP 客户端已存在于 v0.11.3）。

### 📜 出处与许可（本次新增部分）

- **Cinux — MIT License**（`LICENSE` 实读确认，Copyright (c) 2026 Charliechen114514）。
  旧 wip 提交的注释误写成 GPL-3.0，本版已改正。MIT 与 GPL-3.0 兼容。
- **PlantOS — MIT License**（实读确认）。ChaN FatFs 的 LFN 规则属于公开规范实现。
- **SpiritFoxOS — 仓库内没有 LICENSE 文件**（实读：根目录无 LICENSE/COPYING，全文搜索
  只有 vendor/monocypher 的 SPDX 行）。SutureOS 早期注释沿用了"SpiritFoxOS GPL-3.0"的说法，
  依据不足，已登记到 `docs/licensing-audit.md` 作为待确认项；本版新增代码只借用其算法结论，
  不新增该仓库的代码拷贝。

### 📦 下载

- 附件 `sutureos.iso`（14149632 字节）
- sha256：`e58b5b73f8bd2fcae6ed8a1bf65d7378350fe518de1bd08f92c91c7f69400aaf`
- 下载直链：https://github.com/Fanqi89/SutureOS/releases/download/v0.15.0-alpha/sutureos.iso
- 发布页：https://github.com/Fanqi89/SutureOS/releases/tag/v0.15.0-alpha
