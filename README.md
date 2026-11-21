# SutureOS

**English** | [简体中文](README.zh-CN.md)

[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-x86__64-lightgrey.svg)]()
[![Status](https://img.shields.io/badge/status-alpha-orange.svg)]()

SutureOS is an experimental **x86_64 monolithic operating system** built by porting and
integrating components from open-source hobby operating systems. Instead of writing every
subsystem from scratch, it takes proven drivers, filesystems, protocol stacks and algorithms,
wires them together with a small amount of original glue code, and verifies each capability on
a real emulator.

The name "缝合怪" (*stitch-monster*) reflects that methodology — and the project's core rule:
**functionality may be ported, the look and feel may not.** The boot presentation and the entire
GUI visual layer (taskbar, window borders, title bar, colors, fonts, pointer, start menu) are
original to SutureOS and are kept frozen.

| | |
|---|---|
| Latest release | **v0.57.0-alpha** (twelve-domain porting completion) |
| Previous release | **v0.56.0-alpha** (Wayland prerequisites from ring 3) |
| Architecture | x86_64, Limine boot protocol (BIOS) |
| Language mix | C + x86_64 assembly, with a Rust `no_std` component |
| License | **GPL-3.0** (upstream components under MIT / Apache-2.0 / BSD-2 / BSD-3 / MPL-2.0 / Unlicense / GPL-3.0 / OFL-1.1) |

---

## Overview

- **Port-first design.** The porting map covers **64 candidate open-source operating-system
  projects** across twelve capability domains. The current tree derives from **32** of them:
  16 are ported (drivers, filesystems, schedulers, protocol stacks, boot code in active use) and
  16 provide on-disk formats, algorithms or mechanism blueprints that were re-implemented from
  their specifications. The other 32 candidates are either covered by another selected
  implementation, pending license review, or reserved as blueprints for future batches.
- **Original UI.** The graphical shell is drawn by SutureOS itself; no upstream desktop
  environment, compositor or skin is imported.
- **Verification over claims.** Every capability listed below has been exercised in QEMU and
  independently checked on the host (image parsers, WAV analysis, pixel diffing, SHA-256).
- **Reproducible build.** Python build scripts plus a checked-in cross toolchain directory produce
  the kernel and a bootable ISO from a clean tree.

---

## Status

**Released:** `v0.57.0-alpha` — twelve-domain porting completion.

**Highlights of this release:**

- Original 64-bit BIOS boot chain (MBR → stage2 → long mode) sharing the same `kernel.elf` as Limine
- Real HPET, HPET-calibrated LAPIC timer, and a generic `time_source` abstraction
- HDA and AC'97 output volume controls; Sound Blaster 16 + 8237 DMA playback
- Synthetic `/proc` and `/sys`
- USB HID keyboard/mouse with hot-plug rescan
- virtio-gpu 2D and VMware SVGA II (vmsvga) 2D backends (opt-in)
- DHCP client and DNS resolver
- Read-only ext4, VimtuFS2 and OS67 (Minix v1) filesystems; writable MayFS

Current build artifacts from the tree: `kernel.elf` ≈ 6.5 MB, `sutureos.iso` ≈ 23.6 MB,
boot self-tests **16/16**, **1421** global symbols with **0 collisions** (144 objects).

**Desktop (v0.59.0-alpha):** the **Rust shell is now the desktop** — `startgui` enters it, and the
original C desktop moved to `startgui-classic` (its own acceptance suites and appearance gate
still run against it). The shell has pseudo-3D Fluent app icons (22 px in the start menu, 14 px
in every title bar: gradient plate + specular highlight + dark rim + drop shadow, one accent per
app), per-window application content (settings rows, live uptime/CPU figures, a logo image
viewer, an editor buffer), real logo alpha blending, and a keystroke queue that no longer drops
characters. A **minimal Wayland server** lives in the kernel: named AF_UNIX endpoints
(`bind`/`connect`), the wl_display/wl_registry/wl_callback/wl_compositor/wl_surface/wl_shm/
wl_shm_pool/wl_buffer/wl_seat subset, a ring-3 client (`run wlclient`) that hands its memfd over
with SCM_RIGHTS, and `wlshow`, which presents the last committed surface on screen. Shell
acceptance **55/55**, desktop **19/19**, per-window content **25/25**, logo **14/14**, Wayland
**28/28**, wlshow **9/9**; both appearance gates report **0 pixels outside live rectangles**,
and the boot presentation is unchanged.

**This round (post-v0.59.0 GUI wrap-up, not released yet):** presentation moved to triple buffering
on a HPET-driven 60 Hz grid (drop / jitter / latency / histogram counters in `guishell swapstat`,
invariant `rendered == presented + skipped + pending`); redraw became damage-driven composition that
is verified against a full repaint pixel by pixel (`damage: identical`; a pointer-only frame repaints
0.4-0.5 % of the screen, a drag frame costs 0-10 ms where the old full redraw cost ~117 ms). Wayland
gained **multiple clients** (one retained surface per connection, plus `run wltwo`, a single process
that opens two connections), a real `wl_keyboard` / `wl_pointer` event stream (press *and* release,
motion / button / leave / frame), `set_buffer_scale` and region/subsurface semantics; the picture viewer **decodes real images from the FAT32 volume** (BMP *and* QOI,
Left/Right browses the files, integer zoom, name / size / scale footer), the start menu grew a fifth row that opens the Wayland
window (the panel height follows the application registry), and the desktop text
editor became a **real editor** (multi-line buffer, caret and arrow keys, **F5** saves to
`/mnt/fat/GUIEDIT.TXT`, verified byte for byte from the host), damage merging moved to a **32x32 tile
grid** (at most 16 rectangles; a quiet 30-frame window measures ~250 tiles / 132 permille with the
oracle still at 0 mismatches), Wayland also gained **placement semantics** (pointer events are converted into surface-local
coordinates from the window rectangle, enter only while inside, keyboard gated by window focus),
a **full US keymap (3936 B) delivered over memfd + SCM_RIGHTS**, `wl_keyboard.repeat_info`, and memfd reference accounting that returns to zero once the buffer / pool / connection are gone, and buffer/pool lifetime rules - with two real
bugs fixed on the way (a leaked connection slot and a desktop pump that always returned early);
and there is a dedicated **Wayland window opened with `F2`** (it shows a hint card instead of the logo when no client has
committed). The look was polished a second time (wallpaper depth and vignette, icon rim light, accent
identity bar, rounded taskbar and menu) and `docs/baseline/base_gui.png` was regenerated - the gate is
still 0 pixels. Still missing: a real vsync source (there is no scan-line interrupt on this machine, so
60 Hz is a wall-clock grid), finer damage merging, per-window keyboard focus, and real Wayland scaling
composition.

---

## Features

**Boot & platform**
- Limine-based boot on BIOS (UEFI El Torito image is included but not yet validated)
- Original 64-bit BIOS chain: 512-byte MBR → stage2 (16/32-bit → long mode)
- Installer to hard disk, overwrite-style system upgrade, persistent user data

**CPU, interrupts, time**
- GDT/IDT, PIC remapping, IOAPIC (ExtINT cascade), LAPIC timers, SMP bring-up
- Real HPET, HPET-calibrated LAPIC, 8254 PIT fallback, generic monotonic time source

**Memory & process management**
- Physical page allocator, kernel heap, slab allocator, 4-level paging
- Ring-3 user mode with per-process address spaces, `execve`, libc, ELF loader
- Pipes, `AF_UNIX` socketpair, `poll(2)`, `memfd` + `MAP_SHARED`, `SCM_RIGHTS` fd passing

**Storage & filesystems**
- ATA PIO, AHCI/SATA, NVMe, virtio-blk, ATAPI CD-ROM, USB mass storage
- ramfs, FAT32 (VFAT long names, multi-volume), ext2 (read/write), SquashFS, ISO9660
- ext4 (read-only), VimtuFS2 (read-only), OS67 Minix-v1 (read-only), MayFS (read/write)
- Synthetic `/proc` and `/sys`

**Networking**
- RTL8139 and e1000 drivers, ARP, IPv4, ICMP, UDP, TCP (active/passive open, retransmission)
- DHCP client, DNS resolver, `wget` over HTTP/1.0

**USB & input**
- EHCI host controller, USB mass storage (BOT), USB HID boot keyboard/mouse with hot-plug
- PS/2 keyboard and mouse

**Graphics, audio, GUI**
- Linear framebuffer console (flanterm) with GB2312 CJK glyphs
- virtio-gpu 2D and VMware SVGA II 2D backends
- AC'97, Intel HDA and SB16 audio output with volume control
- Original GUI: taskbar, start menu, window management, six applications
- Rust `no_std` renderer prototype (`guislint`, `guishell`) with rounded corners,
  acrylic blur, transitions and a real-time clock

---

## Architecture

```
+-------------------------------------------------------------+
| Ring 3                                                       |
|   user libc  |  ring-3 tools  |  Wayland probe (wlprobe)     |
+------------------------------+------------------------------+
| Ring 0                       |                              |
|   syscall layer              |  VFS                         |
|   scheduler (EEVDF)          |  filesystems                 |
|   TCP/IP + DHCP/DNS          |  GUI compositor + apps       |
+------------------------------+------------------------------+
| HAL                                                          |
|   block layer | NIC | framebuffer/GPU | input | audio         |
+-------------------------------------------------------------+
| x86_64: CPU/APIC/ACPI/PCI/RAM/storage/network/display/input |
+-------------------------------------------------------------+
```

Repository layout (abridged):

```
boot/          Limine configuration, linker script, original BIOS boot chain
arch/x86_64/   interrupts, MMU, scheduling, syscalls, ACPI/HPET/LAPIC
kernel/
  drivers/     block, network, USB, audio, framebuffer, virtio-gpu, vmsvga, SB16
  fs/          ramfs and ten filesystem drivers
  gui/         original desktop shell, applications, image decoders
  ipc/         pipes, AF_UNIX sockets, memfd, SCM_RIGHTS
  net/         link layer, ARP/IPv4/ICMP/UDP/TCP, DHCP, DNS
  shell/       interactive shell (87 commands)
  syscall/     syscall dispatch, ELF loader, ring-3 switching
  time/        generic time-source registry
  install/     installer, upgrade, persistence
  vfs/         unified VFS and ramfs
lib/ include/  kernel libraries and public headers
user/          ring-3 libc and tools
rust/          no_std system layer and software rasterizer prototype
scripts/       build scripts and host-side acceptance tests
tools/         cross toolchain and Limine binaries
docs/          porting, licensing, installer, release and roadmap documents
tests/         symbol scan, SquashFS host harness
```

---

## Building

### Requirements

- **clang** (MSYS2 `mingw-w64-x86_64-clang` on Windows) targeting `x86_64-unknown-none-elf`
- **x86_64-elf-ld** and **x86_64-elf-objcopy** (`tools/cross/bin`)
- **NASM**, **xorriso**, **Python 3**
- Optional: **Rust** (`cargo`, `x86_64-unknown-none` target) for the Rust system layer

### Commands

```bash
python3 scripts/build.py           # kernel    -> build/kernel.elf
python3 scripts/build_iso.py       # bootable  -> build/sutureos.iso
python3 scripts/build_boot_disk.py # optional original BIOS chain image
```

The version string has a single source of truth: `include/sutureos/version.h`.
Release order is: bump version → build → build ISO → run acceptance → publish.

---

## Running

### QEMU (BIOS)

```bash
qemu-system-x86_64 \
  -machine pc -m 256 -vga std -display none \
  -serial file:build/serial.log \
  -cdrom build/sutureos.iso -boot d \
  -drive id=adisk,file=build/fat32.img,format=raw,if=ide \
  -smp 2 -no-reboot \
  -monitor tcp:127.0.0.1:45470,server,nowait
```

- `-vga std` provides the linear framebuffer used by the terminal and GUI.
- Generate the test FAT32 image with `python3 scripts/mkfat32_testimg.py`.
- Always terminate QEMU with the monitor's `quit` command before reading the serial log;
  killing the process can truncate the log.
- A convenience wrapper is available: `scripts/qemu_boot.ps1` (serial log, key injection,
  monitor commands, guaranteed clean shutdown).

### VMware Workstation

`scripts/vmware_boot.ps1` creates a disposable VM, attaches the ISO and a raw disk image as a
SATA device, records the serial log and powers the VM off afterwards. VMware requires a
descriptor-only VMDK with a FLAT extent; the script generates it.

### Real hardware / installed system

1. Boot `sutureos.iso`.
2. Run `install` as root.
3. Select the target disk (the first disk is deliberately not preselected), confirm with `yes`,
   optionally set a root password and create a regular user.
4. Reboot from the disk.

Details: [`docs/INSTALLER.md`](docs/INSTALLER.md), persistence: [`docs/PERSISTENCE.md`](docs/PERSISTENCE.md).

---

## Using SutureOS

The boot sequence is fixed: scrolling English kernel log → SutureOS banner → clean terminal
(`sutureos#`). The GUI is entered explicitly with `startgui`.

### Shell

89 commands are available; `help` prints the full list.

| Group | Commands |
|---|---|
| Files & disks | `ls` `cd` `pwd` `cat` `write` `append` `hexdump` `wc` `head` `tail` `grep` `stat` `rm` `mkdir` `rmdir` `touch` `cp` `mv` `mount` `umount` `df` `sync` |
| System | `help` `uname` `free` `date` `ps` `kill` `cpus` `log` `timesrc` `clear` `reboot` `shutdown` |
| Users | `login` `su` `sudo` `whoami` `id` `passwd` `users` `useradd` `userdel` |
| Networking | `ping` `dhcp` `dns` `nslookup` `wget` `tcpecho` `tcpsend` `tcpserve` `tcpapi` `tcpstat` `netstat` |
| Hardware | `usbinfo` `kbdstat` `mouseinfo` `beep` `hdatest` `hdavol` `ac97vol` `sb16test` |
| Display / GUI | `startgui` `startgui-classic` `guislint` `guishell` `wlshow` `vga2d` `vmsvga` |
| Ring 3 | `run` `wlclient` `ushell` `ring3` `ring3test` `unixtest` `shmtest` `fdpass` `systest` `pipetest` |

### GUI

- `startgui` opens the **Rust desktop shell**: rounded windows over a light-acrylic backdrop,
  a taskbar with the SutureOS logo start button and a live clock, and a start menu with
  pseudo-3D Fluent app icons. Windows support dragging, minimize/maximize/restore (a real
  4-frame geometry transition), close, double-click-to-maximize, and keyboard focus.
- Each window shows its own application content: settings rows, live uptime/CPU figures with
  progress bars, an image viewer (checkerboard + the real SutureOS logo at 3x) and an editor
  buffer with numbered lines.
- `startgui-classic` opens the original C desktop with six applications (Task Manager,
  Calculator, About SutureOS, Image Viewer, Text Editor, Switch User); its appearance baseline
  is frozen separately in `docs/baseline/base_gui_classic.png`.
- `guislint` renders a single frame of the Rust UI prototype; `guishell` runs the same shell
  directly. `wlshow` presents the last committed Wayland surface, and the image-viewer window
  paints that same surface directly (run `wlclient` first). **`F2`** opens the dedicated Wayland
  window (its footer lists every retained client surface), and `run wltwo` drives two clients at
  once. `guishell swapstat` prints the present / triple-buffer state / drop / jitter / latency /
  oracle / damage counters.
- In `guishell` the focused window also owns the keyboard: typed ASCII goes into its editable
  line, `Enter` commits it to the serial log, `Backspace` edits and `Tab` cycles the focus
  between visible windows; window shadows are alpha-composited over whatever is underneath.
- The original SutureOS logo (`logo/` asset ladder, compiled by `scripts/mklogo_assets.py`
  into `kernel/gui/logo_assets.c`) is used by the Rust shell: 24 px icon in the start button,
  48 px icon in the start-menu header, and the 768 px step for the shutdown animation.
  The PNG ladder's alpha channel is preserved and blended in the shell. Press **`F10`** in
  `guishell` to play the reverse logo animation and power the machine off (ACPI S5). The kernel
  never decodes PNG at runtime, and the boot presentation is unchanged.

### Users and privileges

Accounts live in `/etc/passwd` and `/etc/shadow`; passwords use PBKDF2-HMAC-SHA256. `su`,
`login`, `sudo` and password management are implemented. Note that `sudo` is a policy layer:
all tasks run in ring 0, so it is not a hardware security boundary.

---

## Storage and filesystems

| Driver | Filesystem | Access | Notes |
|---|---|---|---|
| `xj_vfs` | ramfs | read/write | root filesystem |
| `fatfs` | FAT32 | read/write | VFAT long names, multiple volumes |
| `ext2` | ext2 | read/write | direct/single/double indirect writes |
| `ext4` | ext4 | **read-only** | extent tree; unsupported features rejected by name |
| `squashfs` | SquashFS 4.0 | read-only | gzip only |
| `iso9660` | ISO9660 | read-only | data discs; El Torito boot discs skipped |
| `vimtufs2` | VimtuFS2 | **read-only** | v5/v4/v3 |
| `os67fs` | OS67 (Minix v1) | **read-only** | inode cache algorithm port |
| `mayfs` | MayFS | read/write | checksummed two-slot snapshot |
| `procfs` | synthetic `/proc` | read-only | meminfo, uptime, version, cpuinfo, mounts |
| `sysfs` | synthetic `/sys` | read-only | kernel/ and block/ trees |

Block device numbering: ATA drives 0–1, AHCI 4–7, NVMe 8, ramdisk 9, virtio-blk 10,
ATAPI CD-ROM 12–15, USB mass storage 16.

Mount usage: `mount <fstype> <drive> <mountpoint>`; `sync` flushes all mounted volumes.

---

## Hardware support

| Subsystem | Supported | Emulation parameters |
|---|---|---|
| Audio | AC'97, Intel HDA, SB16 + 8237 | `-device AC97,audiodev=a0`, `-device intel-hda -device hda-duplex,audiodev=a0`, `-device sb16,audiodev=a0` (with `-audiodev wav,...,out.frequency=48000`) |
| Display | linear framebuffer, virtio-gpu 2D, VMware SVGA II 2D | `-vga std`, `-vga none -device virtio-gpu-pci`, `-vga none -device vmware-svga` |
| USB | EHCI, HID keyboard/mouse, mass storage | `-device usb-ehci -device usb-kbd -device usb-mouse` |
| Network | RTL8139, e1000 | `-netdev user,id=n0 -device rtl8139,netdev=n0` |
| Storage | ATA, AHCI, NVMe, virtio-blk, ATAPI, USB BOT | `-drive if=ide`, `-device ich9-ahci ...`, `-device nvme ...`, `-device virtio-blk-pci ...` |

The virtio-gpu and vmsvga backends are strictly opt-in: a default boot never initializes them,
so the frozen terminal and GUI presentation is unaffected.

---

## Upstream sources and licensing

The original porting plan enumerated **64 candidate open-source operating-system projects**
across twelve capability domains (62 of them license-compatible). The current tree derives from
**32** of those candidates:

- **16 ported directly** — drivers, filesystems, schedulers, protocol stacks and boot code that
  are in active use (VimtuOS, Cinux, CoolPotOS, SpiritFoxOS, Phobos, MOOS, MoeOS, MandelbrotOS,
  HanOS, HeloOS, XJ380, NAOS, PlantOS, Lemis, Uinxed-Kernel, HBOS).
- **16 used as format/algorithm/mechanism references**, with the implementation written from
  their specifications (e1ruBSD, XEKernelOS, zeroAOS, SukiOS, myos, AntzOS, LoongOS, token-os,
  MikuOS, LunaixOS, Neumann, Powerint-DOS-386, OS67, MayOS, Deshab-OS, Lnxrm).

The remaining **32 candidates** are accounted for as follows: **8** are functionally covered by
an already-selected implementation (the project keeps one best implementation per capability
domain), **3** are pending license review or UNVERIFIED (VergeOS-Open, YiYiYa, Nilix), and **21**
are cross-architecture/application mechanism blueprints reserved for future batches. The full
per-source status table is in [`docs/porting-source-map.md`](docs/porting-source-map.md).

In addition, five public specifications are implemented from their text (Linux ext4 on-disk
format, Linux `/sys` conventions, VMware SVGA II, OASIS VirtIO 1.2, USB HID 1.11 / USB 2.0 /
EHCI 1.0), and four non-OS components are used (Limine bootloader, flanterm terminal renderer,
musl reference for the ELF dynamic linker, SIL OFL-1.1 CJK font).

The per-OS module breakdown, per-file provenance and license audit are maintained in:

- [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md) — attribution notices and license texts
- [`docs/provenance.md`](docs/provenance.md) — module → upstream → source path → prefix mapping
- [`docs/licensing-audit.md`](docs/licensing-audit.md) — license audit of upstream repositories

**Known licensing caveat:** SpiritFoxOS, one of the direct sources (slab allocator, TCP/IP
stack, AHCI, network architecture), does not ship a `LICENSE` file; the provenance of those
parts is documented and kept under review.

---

## Testing and quality gates

Each capability has a host-side acceptance script that boots QEMU, injects real keystrokes via
QMP and validates serial output, captured images/WAV files or the resulting disk images.

Three rules apply to every porting batch:

1. QEMU reaches the shell without a kernel panic and the boot self-test count does not decrease
   (currently **16/16**).
2. The capability is independently verified on the host (parsing the disk image, analyzing the
   WAV, pixel-diffing the framebuffer, comparing SHA-256).
3. The GUI appearance gate reports **0 pixels changed outside live-data rectangles**
   (`scripts/gui_baseline_shot.py`).

Additional gates: `tests/symbol-scan.ps1` (global symbol collisions: currently **0** across
1381 symbols) and per-domain acceptance scripts under `scripts/`.

Running a subset:

```bash
python3 scripts/hpet_acceptance.py            # clocks, self-tests
python3 scripts/usb_hid_acceptance.py         # USB HID + hot-plug
python3 scripts/virtio_gpu_acceptance.py      # virtio-gpu 2D
python3 scripts/vmsvga_acceptance.py          # VMware SVGA II 2D
python3 scripts/mayfs_acceptance.py           # MayFS (generate fixture first)
python3 scripts/gui_baseline_shot.py          # appearance gate
```

Notes:

- Filesystem tests need their fixtures: `scripts/mkfs_*.py` / `scripts/mkfat32_testimg.py`.
  The FAT32 image is rewritten by write tests, so regenerate it before running GUI tests.
- QMP key injection must be sent character by character; the acceptance scripts encapsulate this.
- Each QEMU instance should use its own monitor port and disk-image copy when tests run in
  parallel.

---

## Known limitations

- **UEFI boot is not yet validated**; BIOS is the tested path.
- Two schedulers coexist (`task.c` cooperative round-robin and EEVDF); task-context yields are
  deliberately avoided on that path.
- All code runs in ring 0; `sudo` is a policy layer, not a security boundary.
- **USB HID:** high-speed only; boot-protocol devices only; no LED/NKRO; hot-plug polling happens
  in the shell idle loop.
- **virtio-gpu / vmsvga:** 2D only; maximum 1024×768; no 3D, hardware cursor or vsync.
- **SB16:** 8-bit mono 22050 Hz, polled completion.
- **AC'97 volume:** QEMU's WAV backend does not apply mixer gain to the recording; the register
  readback is the evidence. Not validated on real hardware.
- **DHCP/DNS:** A records only; no lease renewal, IPv6, or TCP fallback.
- **ext4 / VimtuFS2 / OS67FS:** read-only and intentionally limited in supported on-disk features;
  details are documented per driver.
- **MayFS:** hard limit of 4095 bytes per file (format invariant); 48 records per volume.
- **GUI visual redesign** is intentionally paused; the original desktop is kept frozen.
- Several drivers are validated only under QEMU; real-hardware validation is ongoing.

---

## Documentation

| Document | Contents |
|---|---|
| [`docs/porting-domains-report.md`](docs/porting-domains-report.md) | Per-domain porting report (modules, licenses, changes, acceptance) |
| [`docs/porting-source-map.md`](docs/porting-source-map.md) | All 64 candidate sources and their current status |
| [`docs/porting-final-summary.md`](docs/porting-final-summary.md) | Summary of coverage, remaining gaps, pending licenses |
| [`docs/provenance.md`](docs/provenance.md) | Module/file-level provenance map |
| [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md) | Attribution and license registry |
| [`docs/licensing-audit.md`](docs/licensing-audit.md) | Upstream license audit |
| [`docs/INSTALLER.md`](docs/INSTALLER.md) | Installer and disk layout |
| [`docs/PERSISTENCE.md`](docs/PERSISTENCE.md) | Persistent user data |
| [`docs/multiuser.md`](docs/multiuser.md) | Accounts and password semantics |
| [`docs/RELEASING.md`](docs/RELEASING.md) | Release process |
| [`docs/porting-roadmap.md`](docs/porting-roadmap.md) | Porting roadmap |
| [`docs/architecture.md`](docs/architecture.md) | Architecture notes |

---

## Contributing

Contributions are welcome. Please keep the following constraints:

- New or ported files must carry a header with **upstream project, source file, verified
  license and a change summary**.
- Assign a module-specific symbol prefix and run `tests/symbol-scan.ps1` (zero collisions).
- Every capability needs a real QEMU run plus host-side independent verification.
- The GUI visual layer and boot presentation must not change; ports may bring functionality,
  not user-interface assets.
- Build and test scripts are Python; write patch scripts to files rather than inline shell
  one-liners to avoid platform quoting pitfalls.
- Open issues and pull requests at <https://github.com/Fanqi89/SutureOS>.

## Security

SutureOS is a hobby/experimental operating system and is **not intended for production use**.
All kernel and user code currently executes in ring 0, and the `sudo` layer is a policy
mechanism rather than a hardware isolation boundary. Please do not deploy it on systems holding
sensitive data. Security reports are welcome via the repository's issue tracker or private
advisory channel.

---

## License

SutureOS is released under the **GNU General Public License v3.0** ([`LICENSE`](LICENSE)).
Upstream components retain their original licenses; see
[`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md) for the complete registry.

## Acknowledgements

Thanks to the authors and contributors of all upstream projects — CoolPotOS, SpiritFoxOS,
Cinux, Phobos, MOOS, MoeOS, MandelbrotOS, HanOS, HeloOS, XJ380, NAOS, PlantOS, Lemis, VimtuOS,
Uinxed-Kernel, HBOS, OS67, MayOS, LunaixOS, Neumann, Powerint-DOS-386, and the authors of the
specifications and reference projects listed in the provenance document.

> *"Standing on the shoulders of giants — even if what we build is a stitch-monster."*
