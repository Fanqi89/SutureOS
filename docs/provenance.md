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
| Slab Allocator | `sfox_` | SpiritFoxOS | GPL-3.0 | `C:\Users\fanqi\Desktop\自研操作系统\SpiritFoxOS` |
| Queue | `phobos_` | Phobos | MIT | `C:\Users\fanqi\Desktop\自研操作系统\Phobos` |
| Red-Black Tree | `cp_` | CoolPotOS | GPL-3.0 | `C:\Users\fanqi\Desktop\自研操作系统\CoolPotOS` |
| FIFO32 | `helos_fifo32_` | HELOS | MIT | `C:\Users\fanqi\Desktop\自研操作系统\HELOS` |
| Scheduler | `sched_` | (local synthesis) | GPL-3.0 | — |
| VFS | `xj_` | XJ380 | GPL-3.0 | `C:\Users\fanqi\Desktop\自研操作系统\XJ380` |
| Boot (multiboot1) | — | Cinux / OSDev | MIT | `C:\Users\fanqi\Desktop\自研操作系统\Cinux` |
| HAL / Context Switch | `cinux_` | Cinux | MIT | `C:\Users\fanqi\Desktop\自研操作系统\Cinux` |
| ACPI | `hanos_` | HanOS | MIT | `C:\Users\fanqi\Desktop\自研操作系统\HanOS` |
| HPET | `hanos_` | HanOS | MIT | `C:\Users\fanqi\Desktop\自研操作系统\HanOS` |
| IOAPIC | `naos_` | NAOS | MIT | `C:\Users\fanqi\Desktop\自研操作系统\NAOS` |

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

---

## License Compliance

| Upstream License | Count | Compatible with GPL-3.0 Aggregate? |
|------------------|-------|-----------------------------------|
| GPL-3.0 | 7 | Yes (same license) |
| MIT | 5 | Yes (permissive) |
| MPL-2.0 | 1 (MandelbrotOS) | Yes — file-level copyleft preserved in `kernel/drivers/mb_pit.c` and `mb_pit.h` |
| BSD-2-Clause | 1 (Limine) | Yes — bootloader separate work, aggregated |

All upstream licenses are compatible with GPL-3.0 for aggregate distribution. MPL-2.0 file keeps its file-level copyleft. Limine BSD-2-Clause is separate binary on ISO.

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