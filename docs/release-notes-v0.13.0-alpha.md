## 🚀 本次更新 (v0.12.0 → v0.13.0)

**e1000 真正通了。** 上一版说「收包没打通」是**判断错了**——驱动一直是好的，是**测试**
有问题：开机自检在中断关闭下跑，轮询循环没法 `hlt` 让出 vCPU，QEMU 设备模型就没机会把
回包 DMA 进 ring；而且自检只给 ping 1000 微秒，一次 `hlt` 就要 10 ms。

现在 `ping 10.0.2.2` 走 e1000 **3/3 回复**，e1000 成为**首选**网卡（RTL8139 回退）。

### 🔧 关键修复

| 现象 | 根因 | 修法 |
|---|---|---|
| 开机自检 ping 超时 / GPRC=0 / 无 DD 位 | 自检时中断关着 → 无法 `hlt` 让出 vCPU → 回包不投递；且预算只有 1000 µs | `ll_yield_once()` 在 IF=0 时 `sti;hlt;cli`；预算 200 ms |
| 回包偶尔"卡"很久才到 | 8254x 在**写 RDT** 时重试排队收包，空闲轮询从不写 RDT | `e1000_receive()` 空闲时重写当前 RDT |
| 横幅错报 rtl8139 | 横幅取 rtl8139 的 IO/IRQ | 按实际驱动打印 |

### ✨ 新增

- **NIC 优先级翻转**：e1000 优先、RTL8139 回退（两条路径都已验证）
- **启动末尾"晚自检"**：每个平台日志里都有一行权威 RX 结论，不用敲键盘

### ✅ 验证

- **QEMU `-device e1000`**：`[net] e1000 at 0xfebc0000 mac=52:54:00:12:34:56`，
  交互 `ping` **3/3 回复**：
  ```
  [sutureos# ping 10.0.2.2]
  PING 10.0.2.2 (ICMP echo, 3 probes)
    64 bytes from 10.0.2.2: icmp_seq=1 time=1852 us
    64 bytes from 10.0.2.2: icmp_seq=2 time=38 us
    64 bytes from 10.0.2.2: icmp_seq=3 time=165 us
  --- 10.0.2.2: 3/3 replies ---
  ```
- **RTL8139 回退全量回归**：早自检 38789 µs、**晚自检 927 µs**、自检 13/14、
  FAT32 写+目录操作 PASS、宿主 17/17、`ring3 9/9`、`userspace 9/9`、AP 在线
- **VMware**：e1000 识别 `0:16.0 dev=0x100f`，MAC `00:0c:29:65:01:af`

### 📦 下载与体验

- 附件 `sutureos.iso`（13.13 MiB，13,768,704 字节）
- sha256：`2fa08d138f8bbba62b6e63913b593a9ffa96e842fb66dc95f438fe7a298e848b`
- QEMU（e1000 首选）：`qemu-system-x86_64 -cdrom sutureos.iso -m 512 -smp 2 -device e1000,netdev=n0 -netdev user,id=n0`
- 进 shell 后 `ping 10.0.2.2` 应 3/3

### ⚠️ 已知限制

- **VMware 上真正联网还缺 DHCP 客户端**（VMware NAT 是 192.168.x 网段，本版硬编码的
  10.0.2.x 是 slirp 的地址）
- e1000 仍是轮询驱动（无中断），够用但可以改成中断驱动