# PS/2 滚轮（v0.65）

本文记录 SutureOS 把 PS/2 鼠标滚轮接进桌面的实现、实测证据与边界。

## 1. 为什么之前没有滚轮

`kernel/drivers/mb_mouse.c` 在 v0.65 之前把包长度写死为 3 字节（`g_packet[3]`、`g_packet_len < 3`）。即使设备已经报告自己是 wheel mouse（id=3），它也会按 **4 字节**发包；第 4 字节包含 Z（滚轮）半字节与额外按键，被驱动当成下一包的首字节 → 整条字节流失去同步，滚轮事件被 `flags & 0x08` 检查丢弃。

## 2. 现在的实现

1. **握手**（`mbos_mouse_init` 第 3b 步，紧接着 `0xF6` 默认值 / `0xF4` 开上报之后）：
   `0xF5` 停止上报 → `0xF3`+`200`、`0xF3`+`100`、`0xF3`+`80`（IntelliMouse 魔术采样率）→ `0xF2` 读设备 id。
   id 为 3（滚轮）或 4（5 键）→ `g_packet_size = 4`；否则保持 3。随后恢复采样率 100、分辨率 0、1:1 缩放并 `0xF4` 重新开上报。
2. **解码**：`mouse_handle_packet(p, len)`；`len >= 4` 时 `z = (int8_t)p[3] >> 4`（4 位补码），`g_wheel += z`（±64 夹紧），并打 `[mouse] wheel z=<n>`。
3. **消费**：`mbos_mouse_take_wheel()` 返回并清零（每帧一次）；`slint_bridge.c` 把它作为 `suture_gui_shell_step(..., wheel)` 的新参数交给 Rust 外壳。
4. **语义**：PS/2 的 Z 约定是 **负 = 远离用户（上滚）**。菜单打开时一格一行（单帧最多 3 格，`menu_scroll_by(±1)`）；图片查看器拥有焦点时上滚=上一张文件、下滚=下一张。日志 `[gui-shell] wheel <delta> menu=<0/1>`。
5. **自检**：`mouseinfo wheel` 把三个合成 4 字节包喂进**真实解码器**（z=+1 / z=−1 / 两个 +1 累加），断言 `take_wheel()` 得到 1、-1、2，打印 `[mouse] wheel selftest: z=+1, z=-1, accumulate -> PASS`。

## 3. 实测证据（QEMU 11.1，`-display none`）

| 探针 | 发送 | 结果 |
|---|---|---|
| probe A（`build/probe_wheel.py`） | 3×wheel-down + 2×wheel-up | 启动日志 `device id=0x3 (wheel) packet=4 bytes`；`packets` 增加、`wheel` 计数 > 0 |
| probe B（`build/probe_wheel2.py`） | 先 2×up 再 3×down | 只收到 **2** 条 `[mouse] wheel z=-1`（即 up）；down 一条也没有 |
| probe C（`build/probe_wheel3.py`） | 只发 3×down | **0** 条滚轮事件（packet 计数不增加） |
| probe D（`build/probe_wheel3.py up`） | 只发 3×up | 收到 `z=-1` 事件 |

结论：**QEMU 的 PS/2 模型只投递 wheel-up，且方向映射为 z=−1；wheel-down 不产生任何包**。因此：

- 端到端只验证了「上滚」这一方向；
- 「下滚」方向由 `mouseinfo wheel` 的合成包覆盖（同一解码器、同一 take 接口）；
- 真机 PS/2（以及 USB HID 翻译的 PS/2 滚轮）未测试，不外推。

QEMU 侧注入方式：这一版 QMP 的 `input-send-event` **没有 `rel` 轴（axis 枚举里没有 wheel）**，滚轮是按钮事件
`{"type":"btn","data":{"down":true,"button":"wheel-up"}}`（还有一个 `wheel-down`，但它到不了 guest）。按下后需要再发一次 `down:false`，否则下一次同名事件会被吞掉。

## 4. 边界

- 离散一格一行；无平滑/惯性/加速。
- 单帧最多消费 3 格（重绘抖动时避免一次跳太远）。
- 菜单滚轮只在菜单打开时生效；图片查看器只在它拥有焦点时吃滚轮；编辑器尚无滚动。
- 驱动假定 VGA/PS/2 兼容的 8042 aux 端口（与既有鼠标驱动相同），不覆盖 USB HID 鼠标（那走 `usbhid` 的独立路径）。
