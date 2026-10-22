## 🚀 本次更新 (v0.6.1 → v0.6.2)

修掉两个"键盘/桌面不能用"的实感问题：**打字丢字**和**进桌面几秒后卡死**。

### 🛠 关键修复

- **键盘丢字 / 整词丢失**：8042 的输出缓冲是键盘和鼠标共用的，而 `mbos_kbd_poll()` 遇到
  aux（鼠标）字节时直接 `return -1`、**不读取**——缓冲于是一直满着，后面的键盘字节永远
  读不到，打字就失灵了；偶尔某个 IRQ1 处理把该 aux 字节当扫描码读掉才"恢复"。
  **在已发布的 v0.6.1-alpha 上实测复现**：提示符下输入 `startgui` 完全没有输入（串口日志里
  只有 `sutureos#` 和后来单个 `q`）。
  修法：`mbos_kbd_poll()` 现在把 aux 字节读出来交给鼠标解码器（新增
  `mbos_mouse_feed_byte()`），只返回扫描码。
  验收：`help` / `df` / `cpus` / `ls` 逐个完整回显，首字符不再丢。

- **进桌面约 4 秒后卡死**（帧停住、`g_tick_count` 仍在走，桌面不再响应，看起来就像键盘坏了）：
  GUI 帧循环里调了 `sched_yield()`，而 `task.c` 的协作轮转与 EEVDF 调度器共用上下文，
  yield 会切到陈旧的 EEVDF 上下文再也回不来。
  实测：修复前帧停在 `frame=400` 而 tick 继续增长；删掉 yield 后跑到 `frame=5700+`、
  按 `q` 正常退出、退出后终端继续可打字。`gui_pace()` 本身已等一个 tick，足够控速。

- **鼠标被初始化两次**（开机一次、进 GUI 又一次）：第二次读配置字节（`0x20`）读到的是待取的
  **数据**字节（实测 `0x9e`），`0xF6` 也没有 ACK，会把鼠标包序列搞乱。进 GUI 现在只查询
  `mbos_mouse_present()`。

- **构建脚本把 ROOT 硬编码成主仓库绝对路径**：在 git worktree 里构建会编译**主仓库源码**并写进
  主仓库 `build/`，留下过期目标文件被后续链接（表现为偶发的 `[SMP] starting AP 1` 之后卡死）。
  改成按脚本位置推导，支持 `SUTUREOS_ROOT` 覆盖。

### ✨ 新增（诊断）

- `[kbd] key #n sc=0x.. ch=0x..`（只打前 12 个键）与 `[gui] frame=.. tick=.. ps2stat=0x..` 心跳
  （每 1000 帧）：串口日志里能区分「字节没到 / 解码成 0 / 循环死了 / 定时器死了」，
  这次两个 bug 都靠它们定位。

### ✅ 本次验证

- 键盘：`help` `df` `cpus` `ls` 全部完整回显；进 GUI 后 `q` 被识别（`sc=0x10 ch=0x71`）并退出；
  退出后终端继续可打字
- GUI：帧心跳跑到 `frame=5700+` 不停；指针自检（点开始按钮 → 菜单向上弹 → 点 Task Manager）全通过
- 回归：自检 13/14、FAT32 写+目录操作自检 PASS、宿主校验 17/17、`[SMP] AP 1 online`、RTL8139 ping 通
- ISO 内版本串：`0.6.2-alpha` = 3 次、`0.6.1-alpha` = 0 次

### 📦 下载与体验

- 附件 `sutureos.iso`（12.58 MiB，13,187,072 字节）
- sha256：`10f043c29ad2707c04af76e731dcc12e4e34dd0d2decd2b64b92bdf9ed370075`
- QEMU 快速体验：
  ```
  qemu-system-x86_64 -cdrom sutureos.iso -m 512 -smp 2 \
      -device rtl8139,netdev=n0 -netdev user,id=n0
  ```
- 进系统后：`help` 看命令，`startgui` 进桌面（鼠标点左下角开始按钮，`q`/ESC 退出桌面）

### ⚠️ 已知限制

- 本机代理拉不动 12.5 MB 整包回读（默认以 API `digest` 为权威证明，`-DownloadBack` 可慢速三向校验）
- 功能限制同 v0.6.1-alpha：VMware 默认 e1000 无驱动（无网络）、真实鼠标移动未自动化验证、
  TCP 无 SACK/窗口缩放/拥塞控制、FAT32 仅 8.3 短名、VimtuFS2 未实现
