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

Total global symbols: **214** — **0 collisions** (verified by `tests/symbol-scan.ps1`).

---

## Source Code Policy

- **Upstream sources**: Read-only, never modified in-place (`C:\Users\fanqi\Desktop\自研操作系统\*`)
- **SutureOS copies**: Modified copies under `C:\Users\fanqi\Desktop\缝合怪系统\kernel\*` and `arch\*`
- **Patches documented**: Each port file header lists upstream source, license, and changes made
- **Git history**: Will track SutureOS modifications only; upstream referenced via provenance matrix