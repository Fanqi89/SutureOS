# virtio-gpu: demo output and the system framebuffer

SutureOS carries one virtio-gpu driver (`kernel/drivers/virtio_gpu.c`) with two
mutually exclusive modes that share the same control queue and the same
modern-PCI bring-up path (common config BAR, notify BAR, ISR BAR, device status,
one descriptor queue of 8 entries).

| Mode | How it starts | What owns scanout 0 | Screen content |
|---|---|---|---|
| demo (v0.57) | `vga2d on` in the shell | `VGPU_RESOURCE_ID` | an animated 64x64 square, ~50 fps |
| framebuffer (v0.70) | automatic at boot when there is no GOP framebuffer | the same resource, 1024x768…1920x1080 | the whole desktop: terminal, fbcon, GUI |

`vga2d on` refuses to run while the framebuffer mode is active and prints
`[vga2d] framebuffer mode owns scanout 0; demo disabled`.

## Boot path

`limine_parse_fb()` in `kmain` returns `addr == 0` on `-vga none` because QEMU's
BIOS leaves no linear framebuffer behind. That is the only trigger for the
framebuffer mode:

1. `virtio_gpu_fb_init()` scans the PCI bus for `1af4:1050` (modern) or the
   legacy `1af4:1110` ID, brings the device to `DRIVER_OK`, reads
   `GET_DISPLAY_INFO`, clamps the reported size to the static backing limit
   (1920x1080) and then to the **1024x768 design size** (a device reporting 0
   falls back to 1024x768). The second clamp is v0.70.1: QEMU 11.1's
   `virtio-gpu-pci` reports 1280x800, and the Rust shell indexes fixed 1024x768
   buffers, so adopting that size made `startgui` panic. With the clamp the host
   window shows the guest frame in its top-left corner.
2. It creates the 2D resource, attaches a backing of exactly `w*h*4` bytes from
   `g_fb[]` (8 MiB, 4096-aligned `.bss`), sets scanout 0 and clears the surface.
3. `kmain` copies the geometry into `limine_fb_addr/width/height/pitch`, sets
   `bpp = 32`, and calls `fbcon_init()`. Every later console write, timer tick
   and GUI frame goes through the same 32-bpp path as `-vga std`.

Serial log when this works:

```
[vgpu] display 1280x800 -> framebuffer 1024x768 (cap 1920x1080)
[vga2d] resource created id=1 1024x768 b8g8r8a8
[vga2d] backing attached addr=0x5af000 bytes=3145728 pitch=4096
[vga2d] scanout 0 <- resource 1
[vgpu] framebuffer ready: addr=0x5d3000 1024x768 pitch=4096 bpp=32
[init] virtio-gpu framebuffer 1024x768 pitch=4096 bpp=32
[init] fbcon initialized (virtio-gpu scanout)
```

## Present model

Pixels are written into `g_fb[]` (normal cached RAM). The host only sees them
after a `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH`, so the API is split by
context:

| Function | Context | Cost |
|---|---|---|
| `virtio_gpu_fb_present()` | thread / shell idle | full-frame transfer + flush |
| `virtio_gpu_fb_present_rect(x,y,w,h)` | thread / shell idle | transfer + flush of one rect |
| `virtio_gpu_fb_mark_dirty(x,y,w,h)` | IRQ safe | records a union rect, no commands |
| `virtio_gpu_fb_service()` | shell idle loop | deferred present of the dirty union |

IRQ handlers only ever call `mark_dirty`; a command queue is never touched from
an ISR. `vgpu_trylock()` (`__atomic_test_and_set`) lets the service path skip a
present instead of spinning on the command lock while the main thread is
mid-frame.

Chrome-side hooks (all guarded by `virtio_gpu_fb_active()`):

- `console_putc()` — present on `\n`, mark dirty otherwise (boot output and
  typed characters both appear before the idle loop ever runs)
- `fbcon_init()` — present after the initial clear; `fbcon_timer_tick()` marks
  dirty only
- `term_clear()`, `wl_view` show, `slint_bridge` after each rendered frame /
  interactive step / shell step — full present
- `gui_render()` — one `present_rect` per frame over the dirty union, then
  clears the flags

## Launch

```bash
qemu-system-x86_64 -machine pc -m 256 -display none \
  -vga none -device virtio-gpu-pci -nic none \
  -drive file=build/sutureos.img,media=cdrom,readonly=on -boot d \
  -serial file:build/serial.log -no-reboot -no-shutdown
```

Screenshot from the monitor (or QMP):

```
screendump /tmp/sutureos.ppm
```

`-vga std` keeps working exactly as before: the driver prints no `[vgpu]` line
and `fbcon` uses the GOP framebuffer.

## Limits

- one scanout, one resource, one queue; no 3D, no cursor plane, no EDID parsing
- 1920x1080 cap (8 MiB static buffer); larger reported modes are clamped
- the guest frame is **1024x768** (the desktop design size) even on a bigger
  host display: QEMU 11.1 reports 1280x800, so the host window shows the guest
  in its top-left corner with black elsewhere
- 32-bpp `B8G8R8A8_UNORM` only
- the framebuffer mode is a system-wide singleton: `vga2d` demos are disabled
  while it is active


---

## ■ 来源与作者（v0.70 合并说明）

| 项 | 内容 |
|---|---|
| 上游项目 | **ConlyesOS**（作者本人为 SutureOS 提供了这套 virtio-gpu 帧缓冲移植与重绘图标） |
| 交付方式 | 作者把改好的整棵工作树放在 `SutureOS-0.70.0-alpha` 目录交给本项目（2026-11-25） |
| 作者/许可 | 交付文件内**未附许可声明**（作者以直接提供文件的方式同意本项目使用）；许可条款待作者书面确认后补录，本项目按 GPL-3.0 收录并在此如实标注 |
| 本版合并内容 | ① virtio-gpu 帧缓冲模式：`kernel/drivers/virtio_gpu.c/.h` 的 `virtio_gpu_fb_*` 一族 API、8 MiB 静态 backing（1920x1080 上限）、`vgpu_attach_resource`/`vgpu_cmd_flush_rect` 重构、IRQ 侧只标 dirty；② `kmain` 在 `-vga none` 下把 scanout 当作系统 framebuffer；③ `fbcon`/`term`/`serial`/`gui`/`slint_bridge`/`wl_view` 的 present 钩子；④ 六个应用图标的重绘（`rust/suture-gui/src/shell.rs::draw_app_icon`） |
| 未合并内容 | 作者同一棵树里还把桌面整体改成扁平设计（纯色壁纸、不透明窗面、去亚克力/阴影/渐变、任务栏与菜单重绘）。本项目**保留原有的圆角 + 轻亚克力 + Fluent 外观**（按项目红线与用户选择），因此 `FLAT_*` 色板与扁平化绘制代码没有并入 |
| 三方核对 | 交付树中的其余 87 个文件差异经逐字节比对，除行尾/空白归一化外无实质改动（只多 `docs/virtio-gpu.md` 与一个测试用 `sutureos.img`，后者未入库） |

验收见 `scripts/gui_virtio_gpu_fb_acceptance.py`（无 GOP 帧缓冲时 boot → fbcon → `startgui` 桌面全落在 virtio scanout 上）。
