# SutureOS Provenance Matrix

**Kernel:** SutureOS (stitched open-source hobby OS)
**License:** GPL-3.0 (aggregate of upstream licenses)
**Bootloader:** Limine 12.9.3 (BSD-2-Clause)

---

## Module → Upstream Mapping

| SutureOS Module | Prefix | Upstream System | Upstream License | Source Path (Read-Only) |
|-----------------|--------|-----------------|------------------|-------------------------|
| GDT/IDT | `vimtu_` | VimtuOS | GPL-3.0 | `C:\Users\fanqi\Desktop\自研操作系统\VimtuOS` |
| Interrupts | `moos_` | MooOS | GPL-3.0 | `C:\Users\fanqi\Desktop\自研操作系统\MooOS` |
| Keyboard | `mbos_` | MandelbrotOS | MPL-2.0 | `C:\Users\fanqi\Desktop\自研操作系统\MandelbrotOS` |
| PIT Timer | `mbos_` | MandelbrotOS | MPL-2.0 | `C:\Users\fanqi\Desktop\自研操作系统\MandelbrotOS` |
| Phys. Page Alloc | `moe_` | MoeOS | GPL-3.0 | `C:\Users\fanqi\Desktop\自研操作系统\MoeOS` |
| Heap Allocator | `lemis_` | Lemis | GPL-3.0 | `C:\Users\fanqi\Desktop\自研操作系统\Lemis` |
| Slab Allocator | `sfox_` | SpiritFoxOS | **无 LICENSE 文件（待确认）** | `C:\Users\fanqi\Desktop\自研操作系统\SpiritFoxOS` |
| Queue | `phobos_` | Phobos | GPL-3.0 (`atomic/COPYING`) | `C:\Users\fanqi\Desktop\自研操作系统\Phobos` |
| Red-Black Tree | `cp_` | CoolPotOS | MIT | `C:\Users\fanqi\Desktop\自研操作系统\CoolPotOS` |
| FIFO32 | `helos_fifo32_` | HELOS | GPL-3.0 | `C:\Users\fanqi\Desktop\自研操作系统\Helo OS\HeloOS` |
| Scheduler | `sched_` | (local synthesis) | GPL-3.0 | — |
| VFS | `xj_` | XJ380 | Apache-2.0 | `C:\Users\fanqi\Desktop\自研操作系统\XJ380` |
| Boot (multiboot1) | — | Cinux / OSDev | MIT | `C:\Users\fanqi\Desktop\自研操作系统\Cinux` |
| HAL / Context Switch | `cinux_` | Cinux | MIT | `C:\Users\fanqi\Desktop\自研操作系统\Cinux` |
| **NIC e1000** | `e1000_` | **Cinux** (`kernel/driver/net/e1000.cpp`) | **MIT** | `C:\Users\fanqi\Desktop\自研操作系统\Cinux` |
| **FAT32 VFAT LFN (read)** | `lfn_` | **PlantOS** (ChaN FatFs lineage) + SpiritFoxOS algorithm notes | **MIT** (+ SpiritFoxOS 待确认) | `C:\Users\fanqi\Desktop\自研操作系统\PlantOS` |
| ACPI | `hanos_` | HanOS | MIT | `C:\Users\fanqi\Desktop\自研操作系统\HanOS` |
| HPET | `hanos_` | HanOS | MIT | `C:\Users\fanqi\Desktop\自研操作系统\HanOS` |
| IOAPIC | `naos_` | NAOS | BSD-3-Clause | `C:\Users\fanqi\Desktop\自研操作系统\NAOS` |

> 许可证一列按本地 `LICENSE`/`COPYING` **实读**结果填写；逐条证据与待确认项见
> [`licensing-audit.md`](licensing-audit.md)。本表此前把 CoolPotOS 写成 GPL-3.0、
> Phobos 写成 MIT、XJ380 写成 GPL-3.0、NAOS 写成 MIT、SpiritFoxOS 写成 GPL-3.0，
> 均已在 v0.15.0-alpha 按实际文件更正。

---

## Key Provenance Notes

### VFS → XJ380
The `xj_vfs` module provides the virtual filesystem layer (ramfs mount, read/write, path lookup). Ported from XJ380's `vfs.c` with GPL-3.0 license.

### HAL / Context Switch → Cinux
The `cinux_context_switch` assembly routine and related HAL primitives come from Cinux's `context_switch.S` (MIT license). Fixed OOB write in original.

### Physical Page Allocator → MoeOS
The `moe_pmm` first-fit page frame allocator with convergent init loop is ported from MoeOS's `page.rs` (GPL-3.0). Fixed off-by-one and OOB bugs.

### Slab Allocator → SpiritFoxOS
The `sfox_slab` per-CPU slab allocator comes from SpiritFoxOS (GPL-3.0). Fixed inverted double-free check and dead migration code.

### Heap Allocator → Lemis
The `lemis_heap` byte-arithmetic block allocator with split/coalesce is from Lemis (GPL-3.0). Fixed pointer-arithmetic bug that prevented coalescing.

### PIT/Keyboard → MandelbrotOS
The `mbos_pit` and `mbos_kbd` drivers are from MandelbrotOS (MPL-2.0, file-level copyleft). File retains MPL-2.0.

### Red-Black Tree → CoolPotOS
The `cp_rbtree` balanced tree implementation is from CoolPotOS (GPL-3.0).

### Queue → Phobos
The `phobos_queue` lock-free FIFO32 is from Phobos (MIT).

### FIFO32 → HELOS
The `helos_fifo32` ring buffer is from HELOS (MIT).

### Scheduler → Local Synthesis
The `sched` round-robin scheduler is a local synthesis using the Cinux context switch primitive.

### Boot / Multiboot1 → Cinux + OSDev
The multiboot1 header, 32→64-bit transition, and identity paging follow Cinux's `boot.S` structure (MIT) and OSDev wiki recipes.

### ACPI / HPET → HanOS
The `hanos_acpi` and `hanos_hpet` table parsing comes from HanOS (MIT).

### IOAPIC → NAOS
The `naos_ioapic` driver is from NAOS (MIT).

### NIC e1000 → Cinux (v0.15.0-alpha)
`kernel/drivers/e1000.c` / `e1000.h` implement the Intel 8254x NIC (VMware's virtual NIC)
from Cinux's `kernel/driver/net/e1000.cpp` (**MIT**, Copyright (c) 2026 Charliechen114514).
Kept: the polled-RX MMIO-read quirk, `RDT = N-1` sentinel, `RCTL = EN|SECRC|BAM|UPE`, TX
`EOP|IFCS|RS` + `TDT` doorbell. Changed: DMA buffers are plain `.bss` (SutureOS keeps a flat
identity map), and the driver is reached through `ll_send()/ll_poll_rx()` in `kernel/net/net.c`
so RTL8139 stays the first choice and e1000 the fallback.

### FAT32 VFAT long names → PlantOS / SpiritFoxOS (v0.15.0-alpha)
`kernel/fs/fatfs.c` parses VFAT LFN chains: 13 UTF-16 units per entry at the documented byte
offsets, chain checksum validated against the following 8.3 entry (discard on mismatch),
`readdir` reassembles the display name, and `resolve_component()` maps a long name back to its
short alias so `open/stat` work. The rules come from the ChaN FatFs lineage as shipped by
**PlantOS (MIT)**; the "checksum + alias fallback" conclusion was cross-checked against
SpiritFoxOS (which has no LICENSE file - see `licensing-audit.md`). The implementation is a
SutureOS reimplementation inside `fatfs.c`; creating/renaming still produces 8.3 names only.

---

## License Compliance

| Upstream License | Count | Compatible with GPL-3.0 Aggregate? |
|------------------|-------|-----------------------------------|
| GPL-3.0 | 6 (VimtuOS, MoeOS, Lemis, HeloOS, Phobos, local scheduler) | Yes (same license) |
| MIT | 5 (Cinux, HanOS, CoolPotOS, PlantOS, Limine-adjacent boot notes) | Yes (permissive) |
| Apache-2.0 | 1 (XJ380 VFS) | Yes (permissive, patent grant) |
| BSD-3-Clause | 1 (NAOS) | Yes (permissive) |
| MPL-2.0 | 1 (MandelbrotOS) | Yes — file-level copyleft preserved in `kernel/drivers/mb_pit.c` and `mb_pit.h` |
| Unlicense | 1 (MOOS) | Yes (public domain) |
| BSD-2-Clause | 1 (Limine) | Yes — bootloader is a separate binary on the ISO |
| **No license file** | 1 (SpiritFoxOS) | ⚠️ **unresolved** — no redistribution grant; see `licensing-audit.md` §2.1 |

All *declared* upstream licenses are compatible with the GPL-3.0 aggregate. The one open
item is SpiritFoxOS: the repository carries no LICENSE/COPYING, so its derived modules
(TCP/IP stack, RTL8139, AHCI, block layer, fatfs scaffolding) have no documented
redistribution grant yet. This is tracked in `docs/licensing-audit.md`; new code must not
add copies from that repository.

### v0.15.0-alpha / v0.16.0-alpha additions

| Added | Upstream | License | Note |
|---|---|---|---|
| `kernel/drivers/e1000.c/.h` | Cinux `kernel/driver/net/e1000.cpp` | MIT | ported; header now names MIT and credits the author (v0.15.0) |
| `kernel/net/net.c` link-layer dispatch (`ll_send`/`ll_poll_rx`) | local | GPL-3.0 | — |
| `kernel/fs/fatfs.c` VFAT LFN read path | PlantOS (MIT, ChaN FatFs lineage) + SpiritFoxOS algorithm notes | MIT / ⚠️ | reimplemented inside `fatfs.c`; no new SpiritFoxOS code copied (v0.15.0) |
| `kernel/fs/fatfs.c` VFAT LFN **write** path (create/rename/unlink of long names, UTF-16 chain writer + UTF-8 readback) | PlantOS ChaN FatFs `put_lfn()` rules (MIT) | MIT | SutureOS implementation (v0.16.0) |
| `kernel/drivers/ac97.c/.h` | HBOS `src/ac97.c` | GPL-3.0 | PCI probe + NAM + NABM PCM OUT (v0.16.0) |
| `kernel/fs/ext2.c` **write path** | XJ380 `driver/fs/ext4/ext4.cpp` / Deshab-OS `CODE/dsk/ext4.c` (algorithm reference only) | Apache-2.0 | bitmaps, direct/single/double indirect writes, dir insert/remove, rename, truncate (v0.17.0) |
| `kernel/vfs/vfs.c` ramfs capacity | local | GPL-3.0 | 512 nodes / 256 KiB per file, heap chunks (v0.17.0) |
| `kernel/gui/img_bmp.c` | HBOS `src/bmp.c` (algorithm reference) | GPL-3.0 | BMP 8/24/32bpp decode for the image viewer (v0.18.0) |
| `kernel/gui/img_qoi.c` | PlantOS `src/plic/qoi.c` (algorithm reference) | MIT | QOI decoder (v0.18.0) |
| `user/apps/r3*.c` | local (argv frame / dir-fd conventions from VimtuOS) | GPL-3.0 | ring-3 file tools (v0.18.0) |
| `kernel/term/cjk16.c/.h` + `scripts/mkfont_cjk.py` | font asset `ZhengGeDianHei-16.ttf` from HBOS/fonts (name table declares SIL OFL-1.1; Copyright 2026 yzdnn, derived from Fontworks DotGothic16) | OFL-1.1 (font), GPL-3.0 (code) | GB2312 16x16 bitmap subset rendered at build time (v0.20.0) |
| `kernel/drivers/usb_ehci.c/.h` | HanOS `kernel/device/usb/ehci.c` (MIT) as the EHCI skeleton; CBW/CSW + BOT flow cross-checked against HBOS `src/usb_msc.c` (GPL-3.0) | MIT + GPL-3.0 (compatible) | USB 2.0 host controller + mass storage as block device 16 (v0.23.0) |
| `kernel/drivers/hda.c/.h` | PlantOS `src/kernel/drivers/sound/hda.c` (MIT) as the HDA skeleton | MIT (compatible with GPL-3.0) | Intel HDA audio output (v0.27.0) |
| `boot/selfboot/stage1.S` (self-written MBR) | e1ruBSD `sys/arch/amd64/boot/bootsect.S` (BSD-2) chain shape; AntzOS `boot/mbr.asm` (MIT) MBR->PM hand-off; XEKernelOS `src/boot/boot.asm` (MIT) MBR->Stage2 shape | BSD-2 / MIT / MIT | rewritten in GAS, single-section 512-byte MBR that loads stage 2 from LBA 1 |
| `boot/selfboot/stage2.S` (16/32 -> x86_64 loader) | e1ruBSD `stage2.S` (BSD-2) A20/GDT/protected-mode recipe; zeroAOS `versions/v0.10-float/boot.asm` (Apache-2.0) VBE discovery; SukiOS `boot/boot.S` (GPL-3.0) long-mode ordering; myos `boot.asm` (BSD-2) A20/GDT | BSD-2 / Apache-2.0 / GPL-3.0 / BSD-2 | E801 + VBE 1024x768x32 + multiboot1 MBI + ATA PIO kernel load + kernel identity page tables + long-mode entry |
| `boot/boot.S` `_start64` entry + exported page-table symbols | local (same file as the Cinux-derived multiboot1 bootstrap) | MIT (Cinux parts) + GPL-3.0 | 64-bit entry for the selfboot chain; shares pml4/pdpt/pd0/pd_io with the Limine path (v0.57.0) |
| `arch/x86_64/hpet.c/.h` | HanOS `arch/x86_64/hpet.c` (GPL-3.0) as the skeleton; the stub was replaced by a real implementation | GPL-3.0 | ACPI HPET table / 0xFED00000 fallback, MMIO mapping, ENABLE_CNF, atomic 64-bit counter, frequency accessor (v0.57.0) |
| `arch/x86_64/lapic.c` calibration | local (HanOS LAPIC driver) | GPL-3.0 | LAPIC timer now calibrated against HPET; the 8254 mode-3 half-rate error on the PIT fallback path was fixed (v0.57.0) |
| `kernel/interrupts/intr.c` | local (MOOS counter fix) | GPL-3.0 | removed the duplicate per-vector counter (moos_intr self-test now 15/15) (v0.57.0) |
| `kernel/drivers/hda.c/.h` volume | PlantOS `src/kernel/drivers/sound/hda.c` (MIT) skeleton + local output-amp control | MIT | OUT_AMP_CAP probe, SET_AMP_GAIN_MUTE per path node, 0=mute/100=0 dB, `hdavol` shell command (v0.57.0) |
| `kernel/fs/fatfs.c` sync | local (PlantOS/ChaN FatFs lineage) | MIT | `fatfs_sync()` now calls `blk_flush()`, so sync really flushes the device write cache (v0.57.0) |
| `kernel/drivers/sb16.c/.h` | Neumann (GPL-3.0, `kernel/drivers/audio/sb16/sb16.c` + `kernel/drivers/dma/dma.c`) and Powerint-DOS-386 (GPL-3.0, `kernel/drivers/sb16.c` + `dma.c`) read for DSP/8237 register order only; **driver self-written, no code copied** | GPL-3.0 | SB16 DSP (0x220) + 8237 channel 1 PCM, 8-bit mono 22050 Hz (v0.57.0, domain 4) |
| `kernel/fs/mayfs.c/.h` + `mayfs_vfs.c` | format + algorithm reference = MayOS `kernel/fs.c` and `kernel/core.h` (**LICENSE read from the repo: MIT, Copyright (c) 2024 MayOS Project** - the素材库 list said GPL-3.0, the file itself is authoritative); independent re-implementation | GPL-3.0 (ours) / upstream MIT | checksummed two-slot snapshot FS, generation selection, backup-first commit, 4095-byte file cap (v0.57.0, domain 6) |
| `kernel/fs/os67fs.c/.h` + `os67fs_vfs.c` | format spec + algorithm reference = OS67 `inc/minix.h`, `inc/inode.h`, `fs/inode.c`, `fs/bitmap.c`, `fs/dir.c` (Minix v1 layout; read-only reference); independent re-implementation, no OS67 source copied | GPL-3.0 | read-only OS67 inode filesystem: inode cache/refcount/flags, 7 direct + single indirect zones, 16-byte dirents (v0.57.0, domain 6) |
| `kernel/fs/ext4.c/.h` + `ext4_vfs.c` | self-written from the public on-disk spec (Linux Documentation/filesystems/ext4: superblock/group desc/inode/extent tree/dirent); in-repo `ext2.c` used only for code *shape* | GPL-3.0 | read-only ext4 with extent-tree depth 0..4, filetype dirs, sparse holes, fast/slow symlink targets (v0.57.0, domain 6) |
| `kernel/fs/vimtufs2.c/.h` + `vimtufs2_vfs.c` | format spec + reference = VimtuOS `kernel/vfs64.h` / `vfs64.cpp` (**GPL-3.0**, read-only reference); independent re-implementation written from the spec, no VimtuOS source copied | GPL-3.0 | read-only VimtuFS2 v5/v4/v3: superblock CRC, inode CRC, d0..d3/ind/dind, name-in-inode directories (v0.57.0, domain 6) |
| `kernel/drivers/vmsvga.c/.h` | self-written from the public VMware SVGA II register spec + QEMU `-device vmware-svga` external observation; **no third-party source copied** | GPL-3.0 | SVGA II 2D opt-in backend: BAR0 index/value ports, BAR1 VRAM, BAR2 FIFO, SVGA_CMD_UPDATE (v0.57.0, domain 9) |
| `kernel/net/dhcp.c/.h` + `kernel/net/dns.c/.h` shell integration + bounded waits | local (drivers were already in the tree and unmodified; shell/logging/timeout fixes are new) | GPL-3.0 | `dhcp_acquire()`, `[dhcp]`/`[dns]` logging, real timeouts via `net_poll()+MSG_DONTWAIT`, `dns <name>`, wget hostname fallback (v0.57.0, domain 7) |
| `kernel/drivers/usb_hid.c/.h` + `usb_ehci.c/.h` HID hooks | self-written from the USB HID 1.11 App B (boot report formats), USB 2.0 §9 (requests) and EHCI 1.0 §3 (qTD semantics); transport layer reuses the existing `usb_ehci.c` (HanOS MIT + HBOS GPL-3.0, documented in that file) | GPL-3.0 | EHCI HID class driver: boot kbd/mouse decode, interrupt-IN polling, root-port hot-plug rescan (v0.57.0, domain 8) |
| `kernel/drivers/virtio_gpu.c/.h` | self-written from the OASIS VirtIO 1.2 spec (§2.7/§4.1/§5.7) and the in-repo `kernel/drivers/virtio_blk.c` virtqueue reference | GPL-3.0 | modern virtio-pci 2D scanout: GET_DISPLAY_INFO/CREATE_2D/ATTACH_BACKING/SET_SCANOUT/TRANSFER/FLUSH (v0.57.0, domain 9) |
| `kernel/fs/sysfs_vfs.c` | mechanism only: LunaixOS sysfs tree layout, Linux `/sys/block/<dev>/{size,dev,ro}` conventions — **no code copied** | GPL-3.0 (ours) | hierarchical read-only /sys over registered block devices (v0.57.0, domain 6) |
| `kernel/fs/procfs_vfs.c` | mechanism only: XJ380 `driver/fs/vfs/vfs.cpp` procfs node table (Apache-2.0), LunaixOS POSIX-VFS procfs layout (reference), Linux /proc text conventions — **no upstream code copied** | Apache-2.0/MIT (mechanism) + GPL-3.0 (ours) | synthetic read-only /proc: meminfo/uptime/version/cpuinfo/mounts (v0.57.0, domain 6) |
| `kernel/time/time_source.c/.h` | mechanism only: LoongOS (MIT, AArch64 generic-timer/tick abstraction), token-os (MIT, aarch64 bare metal), MikuOS (RISC-V 64 reference) — **no upstream code copied** | MIT (mechanism) + GPL-3.0 (ours) | generic monotonic time-source registry (HPET best source, 100 Hz scheduler tick fallback, ns conversion without overflow) (v0.57.0, domain 11) |
| `kernel/term/flanterm/` + `kernel/term/term.c` | **flanterm** (BSD-2-Clause, Copyright (C) 2022-2025 mintsuki and contributors), vendored via CoolPotOS `src/term/flanterm`; local adapter in `term.c` with documented `SutureOS fix` edits | BSD-2-Clause | framebuffer terminal renderer (v0.9.0) |
| `kernel/syscall/dynlink.c/.h` | reference only: **musl** `src/ldso/dynlink.c` (MIT, Copyright (C) 2005-2020 Rich Felker) — kernel-side ELF dynamic linker written for SutureOS, no musl code copied | MIT (reference) | ELF parse/symbols/relocations/dependency load/TLS (v0.10.x) |
| `kernel/drivers/atapi.c/.h` | Uinxed-Kernel `drivers/block/ata/pata/atapi.c` (Apache-2.0, Copyright (C) 2020 ViudiraTech, 2026 MicroFish); ATAPI PACKET protocol flow rewritten for SutureOS | Apache-2.0 | polled CD-ROM PIO driver, 2048-byte sectors, drives 12..15 (v0.16.0, domain 5) |
| `kernel/fs/ext2.c` write path | algorithm reference only: XJ380 `driver/fs/ext4/ext4.cpp`, **Deshab-OS** `CODE/dsk/ext4.c` (Apache-2.0) — no code copied | Apache-2.0 (reference) | bitmaps, direct/single/double indirect write, truncate, mkdir/rmdir/unlink/rename (v0.17.0) |
| `kernel/syscall/loader.c/.h` + `kernel/syscall/mem.c` + `kernel/ipc/memfd.c` | local; OOM-protection idea only referenced **Lnxrm** (Apache-2.0) / SpiritFoxOS — no code copied | GPL-3.0 (ours) | frame-pool bitmap + free, mmap rollback/-ENOMEM, munmap frame recycling with PTE_MEMFD marker (v0.57.0) |
| `kernel/syscall/loader.c/.h` + `kernel/syscall/mem.c` + `kernel/ipc/memfd.c` | local (OOM-protection idea only referenced Lnxrm/SpiritFoxOS) | GPL-3.0 | frame-pool bitmap + free, mmap rollback/-ENOMEM (no more mapping physical page 0), munmap frame recycling with PTE_MEMFD marker (v0.57.0) |

---

## Symbol Prefix Legend

| Prefix | Module |
|--------|--------|
| `vimtu_` | VimtuOS GDT/IDT |
| `moos_` | MooOS Interrupts |
| `mbos_` | MandelbrotOS PIT/Keyboard |
| `moe_` | MoeOS PMM |
| `lemis_` | Lemis Heap |
| `sfox_` | SpiritFoxOS Slab |
| `phobos_` | Phobos Queue |
| `cp_` | CoolPotOS RB-Tree |
| `helos_fifo32_` | HELOS FIFO32 |
| `sched_` | Scheduler (local) |
| `xj_` | XJ380 VFS |
| `cinux_` | Cinux HAL/Context |
| `hanos_` | HanOS ACPI/HPET |
| `naos_` | NAOS IOAPIC |

Total global symbols: **1381** — **0 collisions** (verified by `tests/symbol-scan.ps1`, 138 kernel objects; ring-3 libc/app objects are separate link units).

---

## Source Code Policy

- **Upstream sources**: Read-only, never modified in-place (`C:\Users\fanqi\Desktop\自研操作系统\*`)
- **SutureOS copies**: Modified copies under `C:\Users\fanqi\Desktop\缝合怪系统\kernel\*` and `arch\*`
- **Patches documented**: Each port file header lists upstream source, license, and changes made
- **Git history**: Will track SutureOS modifications only; upstream referenced via provenance matrix