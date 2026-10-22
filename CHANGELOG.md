# Changelog

All notable changes to SutureOS are documented here.
The version numbers follow [Semantic Versioning 2.0.0](https://semver.org/) and
every release is cut as an annotated git tag `v<version>` whose message also
describes the release. Tags are never moved or deleted, so all versions coexist
in the repository.

## [v0.6.2-alpha]

Keyboard fix release. Typing lost characters (the shell's first keystroke after
boot, and whole words depending on mouse traffic) and the desktop froze a few
seconds after `startgui`, which also made it look like "the keyboard does not
work". Both root causes are documented below.

### Fixed
- **Keyboard starvation (characters/words swallowed)**: the 8042 output buffer
  is shared by keyboard and mouse, and `mbos_kbd_poll()` returned `-1` on an aux
  (mouse) byte *without reading it*. The buffer therefore stayed full and the
  keyboard byte behind it was never read, so typing died until an IRQ1 handler
  happened to consume the aux byte as a scancode. Reproduced on the released
  v0.6.1-alpha: typing `startgui` at the prompt produced no input at all.
  `mbos_kbd_poll()` now drains aux bytes into the mouse decoder
  (`mbos_mouse_feed_byte()`) and returns only scancodes.
- **GUI froze ~4 s after startgui** (frames stopped while `g_tick_count` kept
  advancing, so the desktop looked dead and ignored the keyboard): the frame
  loop called `sched_yield()`, which switches to a stale EEVDF context that
  never comes back. Removed; `gui_pace()` already blocks for one tick.
- **Mouse was initialised twice** (once at boot, again on GUI entry): the second
  `0x20` config-byte read returned a pending data byte (observed `0x9e`) and its
  `0xF6` went unanswered. GUI entry now only queries `mbos_mouse_present()`.
- **`scripts/build.py` / `build_iso.py` hardcoded ROOT** to the main checkout, so
  building from a git worktree silently compiled the wrong sources into the main
  `build/` dir and left stale objects that got linked into a later build (which
  surfaced as an intermittent "hangs after `[SMP] starting AP 1`"). ROOT now
  follows the script location, with `SUTUREOS_ROOT` as an override.

### Added
- `[kbd] key #n sc=.. ch=..` diagnostics (first 12 keys) and a `[gui] frame=..`
  heartbeat every 1000 frames: they separate "the byte never arrived" from "it
  decoded to 0" and "the loop died" from "the timer died" in serial-only runs.

## [v0.6.1-alpha]

Fixup release: the About window was still printing a hardcoded `v0.5.0-alpha`,
so a stale version string was baked into the ISO. Found by the new release
verification, which now also proves the version string is inside the artifact.

### Fixed
- GUI "About SutureOS" reads the version from `include/sutureos/version.h`
  instead of a literal, so the ISO can no longer ship an old version string.

### Added
- `scripts/verify_release.ps1`: proves a release is real - version string inside
  the ISO (and the previous version gone), asset `size`/`digest` read back from
  the API by asset NAME, optional byte-range download-back, every OLD release
  compared against a pre-publish snapshot, and the git side in sync.
- `scripts/publish_all.ps1`: the whole release in one command (snapshot ->
  commit/tag/push -> GitHub Release + ISO -> verification).
- `docs/RELEASING.md` documents the sequence and the checks.

## [v0.6.0-alpha] - unreleased

Kernel hardening on real hypervisors, a much more complete network stack, the
SquashFS filesystem, the boot-presentation rework and a usable GUI pointer.

### Fixed
- **AHCI/SATA on VMware**: the driver treated a lingering `DRQ` bit at command
  completion as a failure (`tf not idle after completion`), which made every
  read fail and left FAT32/ext2/SquashFS unmountable. Completion is now judged
  by `ERR`/`DF` + the fatal `PxIS` bits only, with a bounded settle wait and a
  sticky "device quirk" flag so the wait is paid once.
- **ACPI with a lot of RAM (hang right after `[SMP] BSP LAPIC timer armed`)**:
  with 4 GiB the firmware places the RSDT/MADT above the kernel's identity map
  and the first dereference froze the boot. ACPI structures are now mapped on
  demand (`acpi_touch()`), every length is validated before walking, and the
  MADT subtable walk stops on a zero-length entry instead of spinning.
- Terminal clear actually clears: flanterm does not implement `ESC[2J`, so the
  clear was a no-op; it now erases row by row (`ESC[<row>;1H` + `ESC[2K`), flushes
  the pending double buffer first, and wipes the framebuffer pixels that the
  early fbcon log left behind.

### Added
- **TCP passive open** (`tcp_listen`/`tcp_listen_accept`) plus a one-slot
  out-of-order reassembly buffer, verified by three independent acceptance
  paths (guest as client 7/7 + 6/6, guest as server through QEMU `hostfwd` 8/8,
  and an in-kernel injected-segment self-test `tcpooo`).
- **Socket ABI for `SOCK_STREAM`**: `socket/bind/listen/accept/connect/send/recv/
  shutdown/close` now route to the TCP stack; `tcpapi` echoes through that ABI
  only (8/8).
- **SquashFS 4.0 read-only driver** (gzip data blocks, compressed and raw
  metadata, fragments, id table, fast/extended symlinks), with a shared RFC1951
  inflate extracted from the deb code. Verified by a self-written image
  generator, an independent verifier (83/83) and a host harness that compiles
  the real driver (138/138).
- **Boot presentation**: Limine boots straight into the kernel (no menu), the
  English log scrolls, then the screen is cleared and a SutureOS banner is
  printed before the shell prompt; `startgui` enters the desktop.
- **Console log ring** (128 lines) and `log` / `dmesg` commands that auto-scroll
  to the newest line.
- **PS/2 mouse driver** (8042 aux port, polled), an on-screen arrow pointer, the
  taskbar system name turned into a **start button whose menu pops upwards**
  (Task Manager / Calculator / About SutureOS / Exit to terminal), clickable
  title-bar buttons and client-area clicks forwarded to apps.
- `mouseinfo` diagnostics (8042 status, config byte, device id, packet counts)
  and `startgui test` (pointer self-test through the real packet decoder).
- Host-side harnesses: `vmware_boot.ps1`, `vmware_ui_probe.ps1`,
  `tcp_server_acceptance.py` and a single-sourced version header
  (`include/sutureos/version.h`, also parsed by the ISO manifest).

### Changed
- FAT32 ops self-test now also covers `mkdir`/`rmdir`, `rename` and `unlink`
  (host verifier extended to 17 checks, including "no orphaned cluster chains").

## [v0.5.0-alpha]

Snapshot that the repository started from: GUI framework with Task Manager /
Calculator / About, TCP/IP + UDP/ICMP, several filesystems, SMP bring-up and the
Limine-based ISO. (Tagged retroactively on the commit whose message reads
"完成GUI、TCP/IP、多文件系统等重大更新，升版至 v0.5.0-alpha".)

## [v0.1.0-alpha]

Initial commit: bootable 64-bit kernel with 12/12 self-tests passing.
