# USB HID（EHCI boot 键盘/鼠标）+ 热插拔重扫

移植批次 **B15**（`docs/porting-roadmap.md` 里登记为「USB 热插拔重扫 + EHCI HID」）。
上一批只做了 EHCI + USB 大容量存储（BOT），本批补上 HID 类驱动与 root port 热插拔。

## 1. 交付文件

| 文件 | 说明 |
| --- | --- |
| `kernel/drivers/usb_hid.c`（新） | HID 类驱动：描述符遍历、boot 报告解码、热插拔策略、事件/字符队列 |
| `kernel/drivers/usb_hid.h`（新） | 对外 API（`usb_hid_*`）、事件结构、设备种类常量 |
| `kernel/drivers/usb_ehci.c` | 加「HID hooks」段：端口复位/交还、地址分配、控制传输、中断 IN 传输、PORTSC 访问；`usb_qh_arm()` 改为接收 qTD 指针；新增细分延时 `usb_delay_us()` |
| `kernel/drivers/usb_ehci.h` | PORTSC 位 + 11 个 hook 原型 |
| `kernel/shell/shell.c` | `usbinfo` 命令（`scan` / `watch` 子命令）；shell 空闲循环调 `usb_hid_idle()`；`console_getc()` 增加 USB HID 字符来源 |
| `kernel/shell/shell.h` | `shell_cmd_usbinfo()` 原型（放在命令表定义之前的原型区） |
| `kernel/main/kmain.c` | `usb_probe()` 之后调 `usb_hid_init()`（一行 + 注释） |
| `scripts/usb_hid_acceptance.py`（新） | 两阶段真 QEMU 验收（宿主侧独立 re 解析串口日志） |
| `scripts/qemu_boot.ps1` | 新增可选参数 `-AfterMonitorSeconds`（默认 0 = 原行为），热插拔验收要在 device_add 之后给 guest 一点时间 |
| `scripts/build_iso.py` | 修 bug：`msys_path` 硬编码 `c:/sutureos_iso_temp/iso_root`，与 `temp_dir` 会漂移（worktree 独立临时目录下 xorriso 会指向不存在的目录）；改为由 `temp_iso_root` 推导 |

没有改动：`kernel/gui/*`、`kernel/term/*`、`boot/*`、fbcon、自检清单（仍是 **16/16**，没有新增启动自检项）。

## 2. 移植来源与许可

**自写驱动，没有拷贝第三方代码**（SutureOS 本体 GPL-3.0）。只参考规范文本：

- **USB HID 1.11 附录 B**（Boot Interface Descriptors）：8 字节键盘报告
  （修饰键 / 保留 / 6 个 usage）、3 字节鼠标报告（按键 / 有符号 dx / dy）、
  `SET_IDLE`(0x0A, 0 = 只在变化时上报)、`SET_PROTOCOL`(0x0B, 0 = boot 协议)；
- **USB 2.0 第 9 章**：`GET_DESCRIPTOR` / `SET_ADDRESS` / `SET_CONFIGURATION`，
  全部走 `usb_ehci.c` 里已有的控制传输实现（本批没有新增传输代码）；
- **EHCI 1.0 3.x**：被 NAK 的中断 IN qTD 会保持 active，所以「有界超时」就是
  「没有报告」——驱动把超时当 **NO DATA**，不是错误。

传输层复用既有的 EHCI 代码；那部分的上游出处（HanOS MIT 骨架 + HBOS GPL-3.0 的
CBW/CSW 参考）已经写在本目录 `usb_ehci.h` / `usb_ehci.c` 的文件头，本批没有改变它。

## 3. 对外 API（`kernel/drivers/usb_hid.h`）

```c
int  usb_hid_init(void);                   /* 认领当前插着的 HID（幂等、懒执行） */
int  usb_hid_scan(void);                   /* 热插拔重扫：只读 6 个 PORTSC，有变化才枚举 */
int  usb_hid_present(void);                /* 1 = 至少一个 HID 设备 */
int  usb_hid_count(void);                  /* 已认领数量 */
int  usb_hid_poll(usb_hid_event_t *out);   /* 1 = 有事件, 0 = 没有, -1 = 无控制器 */
const char *usb_hid_name(int i);           /* "usb-kbd(addr=4,ep=0x81)" */
int  usb_hid_kind(int i);                  /* USB_HID_KIND_KEY/MOUSE/OTHER */
void usb_hid_idle(void);                   /* shell 空闲循环钩子（不轮询时为 1 次比较） */
int  usb_hid_take_char(void);              /* 解码出的字符（shell 输入环用） */
int  usb_hid_last_event(usb_hid_event_t *out);
```

`usb_hid_event_t`：`kind`（KEY/MOUSE）、`index`（设备槽）、`keycode`（HID usage）、
`mods`、`ascii`（可打印才非 0）、`buttons`、`dx`、`dy`。

## 4. 串口日志参考（启动时只有接了 HID 才会有这些行）

```
[usb-hid] kbd addr=4 iface=0 ep=0x81 mps=8
[usb-hid] device addr=4 vid=0627 pid=0001 port=1 kind=kbd
[usb-hid] mouse addr=5 iface=0 ep=0x81 mps=4
[usb-hid] device addr=5 vid=0627 pid=0001 port=2 kind=mouse
[usb-hid] hid addr=6 iface=0 class=3 sub=0 proto=0 report descriptor len=74 read=64 (not boot: reports are not decoded)
[usb-hid] device addr=6 vid=0627 pid=0001 port=3 kind=other
[usb-hid] qtd token=80008d00 len=8 residual=usable (0)      # 每次开机一行：控制器是否回报残留字节数
[usb-hid] key down=0x04 ascii=a mods=0x00                    # 前 32 条
[usb-hid] mouse dx=6 dy=6 btn=0                              # 前 32 条
[usb-hid] hotplug: new HID on port 4
[usb-hid] unplug: port 2 (1 HID device(s) released)
```

没有 EHCI 控制器时：**一行都不打**（`usb_hid_init()` 在 `usb_ehci_ready()==0` 时直接返回），
所以没有 USB 的机器启动日志与改动前逐行一致。

## 5. 验收

```bash
py -3 scripts/build.py
py -3 scripts/build_iso.py
py -3 scripts/usb_hid_acceptance.py
```

`scripts/usb_hid_acceptance.py` 跑两次真 QEMU，宿主侧用正则独立解析串口日志：

阶段 1（`-device usb-ehci` + `usb-kbd` + `usb-mouse` + `usb-tablet`）
1. 键盘/鼠标在启动时被认领 → `[usb-hid] kbd addr=N iface=0 ep=0x81 mps=8`（mouse 同理）；
2. `usb-tablet` 不是 boot 子类 → 打印 report descriptor 长度而不解码；
3. 打字 `usbinfo` → `usbinfo: 3 HID device(s)` + 按名字列出键盘；
4. `sendkey a`（按住 400 ms）→ `[usb-hid] key down=0x04 ascii=a mods=0x00`；
5. HMP `mouse_move 6 6` → `[usb-hid] mouse dx=6 dy=6 btn=0`；
6. `device_add usb-kbd,id=kbd2,bus=ehci.0` → `[usb-hid] hotplug: new HID on port 4`。

阶段 2（`usb-ehci` + 两个 `usb-kbd`）
7. `device_del kbd2` → `[usb-hid] unplug: port 2 (1 HID device(s) released)`。

稳定性：`[stitch] selftests: 16/16 passed`、无 PANIC（两阶段都查）。

实测（2026-11-20，本 worktree）：

```
[usb-hid] checks=14 ok=14 fail=0
[usb-hid] RESULT: PASS
```

## 6. 集成到主仓（文件级）

1. 复制新文件：`kernel/drivers/usb_hid.c`、`kernel/drivers/usb_hid.h`、
   `scripts/usb_hid_acceptance.py`（`build.py` 自动 glob `kernel/**/*.c`，不用改构建脚本）。
2. `kernel/drivers/usb_ehci.h`：文件头注释补一段；加 PORTSC 位
   （`USB_EHCI_PORTSC_CCS/CSC/PED`）与 11 个 `usb_ehci_*` hook 原型（放在
   `usb_selftest()` 之后）。
3. `kernel/drivers/usb_ehci.c`：
   - `#include "time_source.h"`（在 `mb_pit.h` 之后）；
   - DMA arena 里加 `g_intr_qtd` / `g_intr[64]` / `g_intr_resid_seen`；
   - `usb_delay_ms()` 前加 `usb_delay_us()`（time_source 细分等待）+ `usb_delay_ms` 前置声明；
   - `usb_qh_arm()` 最后一个参数从 `uint32_t qtd_index` 改成 `usb_qtd_t *first`，
     体内 `qh->next_qtd = ...usb_ptr_phys(first)`；两处调用改 `&g_qtd[0]`；
   - `usb_qh_arm()` 停止异步调度的循环改 `usb_delay_us(100)` / 400 次；
   - `usb_wait_qtds()` 之后加 `usb_wait_qtds_us()`；
   - `usb_block_size()` 之后加整段「HID support hooks」（`usb_ehci_ready` 起 11 个函数，
     约 190 行，含 `usb_ehci_intr_in` 的一次性 residual 诊断）。
4. `kernel/main/kmain.c`：`usb_probe()` 之后加 `extern int usb_hid_init(void); (void)usb_hid_init();`。
5. `kernel/shell/shell.h`：`shell_cmd_guishell()` 之后加 `shell_cmd_usbinfo()` 原型。
6. `kernel/shell/shell.c`：
   - include 区（`mb_kbd.h` 之后）加 `#include "usb_hid.h"`；
   - `shell_cmd_unixtest()` 之后加 `usbinfo_show_event()` + `shell_cmd_usbinfo()`（约 120 行）；
   - 命令表 `timesrc` 之后加 `{"usbinfo", shell_cmd_usbinfo, "..."}`；
   - `console_getc()` 之前加 `shell_hid_fill()`，并在 `console_getc()` 里 PS/2 无输入时调用；
   - shell 空闲循环 `net_poll()` 之后加 `usb_hid_idle();`。
7. `scripts/qemu_boot.ps1`：加 `-AfterMonitorSeconds`（param + monitor 命令后的 dwell + 头注释）。
8. `scripts/build_iso.py`：`msys_path` 改为由 `temp_iso_root` 推导（一行）。

## 7. 已知限制

- **只做高速设备**：EHCI 端口上的 full/low-speed 设备由 `usb_ehci_port_reset()` 失败后
  `usb_ehci_port_release()` 交还伴随控制器（PO=1），和存储驱动一致；不做 split transaction。
- **只解码 boot 设备**：`bInterfaceSubClass=1` 且 protocol 1/2 才解码。<br>
  非 boot HID（例：`usb-tablet`）会被认领、打印 report descriptor 长度，但**不解码报告**
  （需要通用 HID 报告解析器，本批没有）。
- 无 `SET_REPORT`（键盘 LED/大小写灯）、无 NKRO、无 consumer 键、无重复键（typematic）状态机。
- **热插拔靠轮询**：SutureOS 没有 USB 中断路由（`USBINTR` 保持 0），端口变化由 shell 空闲循环
  （每 16 次一轮 PORTSC 读）和 `usbinfo` 命令触发。GUI 里没有 HID 轮询，因此在 GUI 中插拔
  直到回到 shell 才会被发现。
- **HID 键盘字符与 PS/2 是两条来源**：`console_getc()` 先取 PS/2 环，空时再取 HID 字符，
  所以只有 USB 键盘的机器也能打字；但同时接 PS/2 与 USB 键盘的真机上，同一次按键可能
  进两个字符（QEMU 不会，因为它把注入键只发给 USB 键盘——见下）。
- **QEMU 行为（宿主侧事实，不是驱动限制）**：一旦 VM 里有 `usb-kbd`，QMP/HMP 注入的按键
  只投递给 USB 键盘，PS/2 端口**一个字节都收不到**。用一块 guest 完全不碰的 UHCI
  `usb-kbd`（`-usb -device usb-kbd`，SutureOS 没有 UHCI 驱动）验证过：PS/2 键盘计数为 0。
  验收因此把「打字」交给 USB HID 字符通路。
- **未在真机验证**：全部结论来自 QEMU 11.1 的 EHCI/HID 模型；真机 USB 事务 CRC 错误、
  端口去抖（debounce）、`PORTSC` 的 PSPD 位行为都未验证（QEMU 的 PSPD 恒 0，现有驱动
  用「复位释放后 PED=1」判高速）。
- 设备槽上限 4（`USB_HID_MAX_DEV`），事件队列 32，字符队列 16；满了丢最新的，不阻塞。
