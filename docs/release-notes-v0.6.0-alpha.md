## 🚀 本次更新 (v0.5.0 → v0.6.0)

### 🛠 关键修复（都是"真机/虚拟机里跑不起来"的问题）

- **VMware 下 SATA 读盘全挂**：AHCI 在命令完成的那一瞬间 PxTFD 仍显示 `DRQ=0x8`，而驱动把"完成但 Task File 未空闲"直接判成失败 → 端口重启重试 → `sata0 read failed`，于是 FAT32/ext2/SquashFS 一个都挂不上。现在判据只认 `ERR`/`DF` 与 PxIS 致命位，DRQ/BSY 当作设备怪癖容忍（记一次日志 + sticky 标志，后续不再为它付等待）。
- **4 GiB 内存时卡死在 ACPI**：内存大时固件把 RSDT/MADT 放到内核恒等映射窗口之外，启动停在 `[SMP] BSP LAPIC timer armed` 之后、`[ACPI]` 一行都不打。现在读任何 ACPI 结构前先按 2 MiB 做恒等映射（`acpi_touch()`），长度先校验再遍历，MADT 子表遍历遇 0 长度即停。
- **终端"清屏"真的清屏**：flanterm 并不实现 `ESC[2J`，原来发它是静默空操作，日志一直留在屏上。改为逐行 `ESC[<row>;1H` + `ESC[2K` 擦除，先冲刷双缓冲，再擦掉早期 fbcon 直接画进帧缓冲的像素。

### ✨ 新增能力

- **TCP 被动打开**（`listen`/`accept`）+ 一个乱序段重组槽：三条独立验收——guest 当客户端（echo 7/7、bulk 6/6）、guest 当服务端（QEMU `hostfwd`，8/8）、内核内注入段自检 `tcpooo`。
- **套接字 ABI 真正接到 TCP**：`socket/bind/listen/accept/connect/send/recv/close` 按 `SOCK_STREAM` 分流，`tcpapi` 只用这套 ABI 起 echo 服务器（8/8）。
- **SquashFS 4.0 只读驱动**：gzip 数据块、压缩/未压缩元数据、fragment 表、id 表、快/慢符号链接；配套自写镜像生成器、独立校验器（83/83）与在宿主上编译真实驱动的 harness（138/138）。
- **启动呈现重做**：关掉 Limine 菜单，先滚动英文日志，再清屏打印系统名横幅，然后进入干净终端；`startgui` 进桌面。
- **控制台日志环 + `log`/`dmesg`**：128 行环形缓冲，打印后自动滚到最新行。
- **GUI 鼠标与开始菜单**：PS/2 鼠标驱动（开机初始化，轮询）、屏幕箭头光标、点左下角 "SutureOS" 从底部向上弹出开始菜单（Task Manager / Calculator / About SutureOS / Exit to terminal），标题栏三键可点。
- **诊断与自动化**：`mouseinfo`、`startgui test`（指针自检走真实包解码器）、`vmware_boot.ps1`、`vmware_ui_probe.ps1`、`tcp_server_acceptance.py`、`log_ring`。
- **版本工程化**：版本号单一来源 `include/sutureos/version.h`（内核横幅 / `/etc/motd` / ISO manifest 全部取这里）+ `CHANGELOG.md` + `scripts/release.ps1`（提交/打标签/推送）+ `scripts/publish_release.ps1`（发布 Release 并上传 ISO）。

### ✅ 本次验证

- 内核自检 **13/14**（仅既有的 `moos_intr` 判定问题）
- FAT32 写 + 目录操作自检 **PASS**，宿主侧独立校验 **17/17**
- SMP 双核上线（AP 1 online）、RTL8139 ping 通（QEMU slirp）
- ext2 与 SquashFS 自检 **PASS**（可与 FAT32 同时在线）
- **VMware Workstation 17.5.2 实测**：SATA 识别 → FAT32 挂载 → 写/目录自检 PASS；4 GiB 内存配置一路启动到 `sutureos#`
- QEMU 4 GiB 内存 + 四种块控制器 + 三套文件系统同时在线验证

### 📦 下载与体验

- 下载下方附件 `sutureos.iso`（12.57 MiB，13,182,976 字节）
- sha256：`37e50caf4e8d7918327b25add8bd4ca72a533528478b58c046a5dde182e83982`
- QEMU 快速体验：
  ```
  qemu-system-x86_64 -cdrom sutureos.iso -m 512 -smp 2 \
      -device rtl8139,netdev=n0 -netdev user,id=n0
  ```
- VMware / VirtualBox：新建 64 位虚拟机，内存 ≥512 MB、≥2 核，光驱指向该 ISO（BIOS 启动）
- 进系统后：`help` 看命令，`log 200` 回看启动日志，`startgui` 进桌面（鼠标可点左下角开始按钮）

### ⚠️ 已知限制

- **VMware 下没有网络**：VMware 默认虚拟网卡是 e1000，而 SutureOS 目前只驱动 RTL8139（QEMU 用 `-device rtl8139` 正常）
- **真实鼠标移动未自动化验证**：QEMU 无显示后端与 VMware VNC 的指针事件都到不了客人机；8042 aux 数据通路已由 `mouseinfo` 读设备 ID 证实，解码→光标→点击→菜单由 `startgui test` 端到端证实。若真鼠标不动，请跑 `mouseinfo` 看 `packets/obf/aux` 哪一步为 0
- TCP 尚无 SACK / 窗口缩放 / 拥塞控制；FAT32 仅 8.3 短名；VimtuFS2 磁盘格式未实现
- 处于 alpha 阶段，仅供尝鲜，请勿在物理机日常使用
