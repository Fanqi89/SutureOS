# 外观基线（Appearance baseline）

这两个截图是 v0.11.0-alpha（5b35fe2）的启动终端与 GUI 画面，作为"外观冻结"的比对基准。

- base_terminal.png —— 开机完成后进入的终端画面（滚动英文日志 → banner →
  Welcome to SutureOS Terminal / sutureos#）
- base_gui.png —— 在终端输入 startgui 之后的桌面（任务栏、Task Manager、
  Calculator、About 三个窗口、鼠标指针）

## 规矩（用户 2026-10-23 明确）

1. 可以移植其他系统的功能与代码，但不得改变启动方式与 GUI 界面（含任务栏、窗口边框、
   标题栏、配色、字体、指针、开始菜单）。
2. 移植的功能必须有 SutureOS 自己写的界面，不得直接搬对方的窗口/控件/皮肤/布局。
3. 每次改动后按下列口径验收：
   - 终端截图与 base_terminal.png 逐像素相同（版本号升级导致的 banner 变化需显式说明）；
   - GUI 截图只允许 Task Manager 的实时计数器区域 (44,102)-(352,221) 不同，
     其余区域必须逐像素相同；
   - 启动日志逐行对比只允许内核地址、PMM 空闲页数、RTC 时间、版本号字符串不同；
   - 开机自检类输出只在失败时打印，不往启动日志里加行。
