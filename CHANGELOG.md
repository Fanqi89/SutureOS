# Changelog

All notable changes to SutureOS are documented here.
The version numbers follow [Semantic Versioning 2.0.0](https://semver.org/) and
every release is cut as an annotated git tag `v<version>` whose message also
describes the release. Tags are never moved or deleted, so all versions coexist
in the repository.

## [v0.14.0-alpha]

FAT32 gets **VFAT long file names** (read side), stitched from PlantOS (ChaN FatFs) and
SpiritFoxOS rather than Cinux.

### Added
- **VFAT long-name reader** in `kernel/fs/fatfs.c`: the LFN entry state machine
  (attribute byte exactly 0x0F, ord chain with 0x40 on the physically first entry,
  13 UTF-16 code units at offsets 1/3/5/7/9/14/16/18/20/22/24/28/30, SFN checksum
  verification, 0x0000/0xFFFF terminators, 0xE5 deleted / 0x00 end-of-directory).
  Implementation notes taken from PlantOS `src/fs/fatfs/ff.c` (the only sibling repo
  with a full LFN read+write) and SpiritFoxOS `kernel/src/fs/fat32.c` (the ancestor of
  this driver) - with the checksum validation the ancestor is missing.
- **`ls` prints long names**; `path_resolve` falls back to a long-name directory scan
  when a component does not fit 8.3, so stat/open/cat/unlink/rmdir accept long names
  while the rest of the driver stays 8.3-based.
- **unlink/rmdir mark the whole preceding LFN chain 0xE5** instead of orphaning it
  (that would leave invisible stale entries that some OSes still hide behind).
- **Test fixture + host verifier**: `scripts/mkfat32_testimg.py` now writes a real LFN
  chain (long name `Long file name test.txt`, short alias `LFNTRAN.TXT`, checksum and
  reverse ordering included); `scripts/verify_fat32_write.py` skips LFN/volume
  entries when enumerating the root (it parsed them as files with cluster 0 and
  walked off the volume).

### Fixed
- New files were created with attribute 0x01 (read-only); they are 0x20 (archive).
- The LFN/volume test used `(attr & 0x0F) == 0x0F`, which also matches 0x1F/0x2F/0x4F
  (directories with the archive bit set) - a long name could be built for a
  directory. It is now an exact `attr == 0x0F` test.

### Verification
- Guest: `ls /mnt/fat` prints `Long file name test.txt` from the fixture's chain.
- Guest regression unchanged: `[fat32] write self-test PASS`,
  `[fat32] ops self-test PASS (mkdir/rmdir, rename, unlink: 3/3)`, self-tests 13/14,
  `ring3 selftest 9/9`, `userspace selftest 9/9`.
- Host: `py -3 scripts/verify_fat32_write.py build/fat32_ata.img build/fat32.img`
  -> **checks=17 ok=17 fail=0**.

### Notes on the "port everything" line
- **Cinux has no FAT driver at all** (only ext2 plus a TODO document), so the LFN
  source is PlantOS/SpiritFoxOS, not Cinux.
- **MOOS's FAT is stock ChaN FatFs R0.13 vendored inside a managed C# kernel** and is
  dead code there (`//new FATFS();`); it is not a port source. Useful algorithms are
  now on the list instead: SFN `~N` alias generation, LFN **write**, NTres lowercase
  preservation, and the FSInfo trust protocol.

### Not done yet
- LFN **write** (creating a long name from this OS; needs the multi-slot directory
  allocator and the `~N` short-name generator).
- HanOS scheduler port / `fork`+`wait4`.

## [v0.13.0-alpha]

e1000 finished: it is the **preferred** NIC now, because receive works. The v0.12.0
"RX does not deliver" verdict was wrong - the driver was fine, the *test* was not.

### Fixed
- **e1000 RX was never broken.** `ping 10.0.2.2` over `-device e1000` answers
  **3/3** (1852/38/165 us). What failed was the boot-time net selftest, for two
  reasons: (a) it ran with interrupts masked, so the poll loop could not `hlt` and
  QEMU's device model never DMA'd the reply into the ring; (b) it gave the ping only
  **1000 us**, while one `hlt` costs up to a full 10 ms tick. `ll_yield_once()` now
  does `sti;hlt;cli` when IF=0 (net_poll is never called from an ISR) and the boot
  budget is 200 ms.
- `e1000_receive()` re-asserts the current RDT while idle: QEMU retries a queued
  receive packet from the descriptor-tail write handler, and an already-armed ring
  otherwise never writes it.
- The NIC banner printed RTL8139's I/O base and IRQ even when e1000 was the active
  driver ("rtl8139 found at 0x0"); it now names the real driver and address.

### Changed
- **NIC preference is e1000 first**, RTL8139 as the fallback (both directions are now
  proven, so VMware and `-device e1000` get the better path).
- Added a **late network selftest** right before the shell starts (the
  interactive-equivalent state). The log now carries an authoritative RX verdict on
  every platform without typing anything: `[net] selftest ping 10.0.2.2 -> reply in
  927 us` on RTL8139, and the same line on e1000 setups.

### Verification
- QEMU `-device e1000`: `[net] e1000 at 0xfebc0000 mac=52:54:00:12:34:56`,
  boot selftest + late selftest, interactive `ping` **3/3 replies**.
- Full regression on the RTL8139 fallback path: `[net] rtl8139 at 0xc000 irq=11`,
  early selftest reply 38789 us, **late selftest reply 927 us**, self-tests 13/14,
  FAT32 write+ops PASS, host verifier 17/17, `ring3 selftest 9/9`,
  `userspace selftest 9/9`, AP online.
- VMware: e1000 detected (`0:16.0 dev=0x100f`, MAC `00:0c:29:65:01:af`). Note VMware
  NAT uses a 192.168.x subnet, so the hard-coded 10.0.2.x slirp addresses cannot
  produce a reply there - that check needs DHCP (future work).

## [v0.12.0-alpha]

Stitching other operating systems' strengths: the first port that does not come from
VimtuOS. The **Intel e1000 NIC driver** is ported from **Cinux**, together with a
link-layer dispatch so the network stack can sit on either NIC.

### Added
- **e1000 driver** (`kernel/drivers/e1000.c`, ported from Cinux
  `kernel/drivers/net/e1000.cpp`): PCI probe (8086:100e/100f/10d3), MMIO BAR0
  mapping, reset, EEPROM MAC read, RAL0/RAH0/MTA filter programming, legacy RX
  descriptor ring (32 x 2 KiB) and TX ring (8 descriptors, EOP|IFCS|RS), polled
  send/receive. This is VMware's virtual NIC - the card the OS is actually used on
  was previously unsupported.
- **Link-layer dispatch** in `kernel/net/net.c`: `ll_send()` / `ll_poll_rx()` pick
  RTL8139 (QEMU default, fully working) or e1000 (VMware), so the IPv4/ARP/ICMP/UDP
  stack is NIC-agnostic. `net_link_name()` reports which one is active.
- **VMware test VM now has a NIC**: `scripts/vmware_boot.ps1` adds
  `ethernet0.virtualDev = "e1000"`, so the e1000 path is exercised on the real
  hypervisor.

### Verification
- **VMware**: `[e1000] found at 0:16.0 dev=0x100f BAR0=0xfebc0000` +
  `[  OK  ] e1000: MAC=0:c:29:65:1:af link=1 fd=1 RCTL=0x400800a` +
  `[net] using e1000`. Full boot otherwise unchanged (AHCI/FAT32, ring3 9/9).
- **QEMU `-device e1000`**: card detected, MAC read, and **frame transmission
  verified on the wire** with `-object filter-dump` (pcap shows our ARP requests to
  ff:ff:ff:ff:ff:ff and slirp's replies to our MAC).
- **QEMU regression (RTL8139 path)**: unchanged and green - self-tests 13/14,
  `[net] selftest ping 10.0.2.2 -> reply in 2107 us`, FAT32 write+ops PASS, host
  verifier 17/17, `ring3 selftest 9/9`, `userspace selftest 9/9`, AP online, mouse.

### Known limitation (stated plainly)
The e1000 **receive path does not deliver packets yet** on either QEMU or VMware:
descriptor 0 never gets the DD bit and GPRC stays 0, although TX completes
(TDH/TDT advance, DD set) and the pcap proves the frames reach the card. Cinux'
note about the emulator's main loop explains QEMU; it does not explain VMware, so
there is still a ring-setup difference to find. Because of that the dispatch prefers
RTL8139 when both are present - existing QEMU networking is not regressed - and the
e1000 is used only where it is the only NIC (VMware), where it currently gives
detection, MAC and TX.

## [v0.11.0-alpha]

The userspace becomes real: interrupts stay enabled at CPL=3, every program runs in
its own address space (CR3 switch), `execve` replaces a running program, and there is
an interactive ring-3 shell.

### Added
- **Per-process address spaces** (`kernel/syscall/userspace.c`): a real PML4 per
  program - kernel map shared, the 1 GiB..2 GiB slot (free in the kernel map) gets a
  private PDPT/PD/PT for the user window at 0x40000000, so two processes can use the
  same VA and see different pages. CR3 is switched on entry to ring 3 and back on
  exit. Page-table pages come from the PMM (the static MMU pool ran out after a few
  runs), and are released on exit.
- **Ring 3 runs with IF=1**: hardware interrupts are delivered while a program is at
  CPL=3 (the timer keeps ticking), so userspace is preemptible in principle.
- **`execve`** as own-ABI `int 0x80` number 9: rebuilds the current address space
  with another built-in program and rewrites the interrupt frame (rip/rsp/rax) so
  `iretq` lands at the new entry point. Used by the shell's `hello`/`probe` commands.
- **Ring-3 interactive shell** (`user/apps/shell.c`, 6889 B): reads commands through
  `int 0x80` read(fd=0) (the kernel echoes them), and supports help/hello/probe/
  crash/pid/ticks/exec/exit. The kernel shell enters it with `ushell`.
- **Console input for ring 3**: `int 0x80` read on fd 0 is routed to the keyboard
  ring with echo and backspace handling.
- **`userspace_selftest`** (9 checks) wired into the boot log.

### Fixed
- **`el0_enter_user_saved` switched CR3 after zeroing the registers**, which cleared
  `%rdi` (the PML4 argument) - so CR3 was never switched and ring 3 ran on the kernel
  map where 0x40000000 is unmapped (QEMU: CPL=3 #PF err=0x14 at 0x40000000 forever).
- **The window is at 1 GiB, i.e. PML4 index 0** (not 1, which starts at 512 GiB), so
  the private PDPT belongs in `pml4[0]`.
- **#PF from CPL=3** was handled by the COW path, which ended in
  `sig_send(current=-1, SIGSEGV)` and re-executed the faulting instruction forever.
  A user-mode page fault now kills the program (exit 128+14) and the kernel survives:
  `[ring3] user #PF at 0x100000 (err=0x7) -> killed (exit 142)`.
- **GS MSRs are saved/restored across ring 3**: a `syscall`-initiated exit
  (`SYS_EXIT`) long-jumps out of the entry path and skipped the matching `swapgs`,
  leaving GS swapped for the next entry.
- Two shell input bugs: the prompt was reprinted on every `-EAGAIN`, and a single
  keystroke was treated as a command (read(0) returns as soon as one char is
  available, so the shell accumulates until newline).

### Verification
- `userspace selftest: RESULT PASS (9/9)`, `ring3 selftest 9/9`,
  `int80 selftest 2/2`, regression otherwise unchanged (self-tests 13/14, rtc 8/8,
  pipe 16/16, FAT32 write+ops PASS, host 17/17, AP online, ping, mouse, sudo 23/23).
- Interactive ring-3 shell transcript: `help`/`ticks`/`pid` work, `hello` execve's the
  libc program in the same address space, `crash` (write to kernel memory) is killed
  with exit 142 while the kernel survives, and `ushell` can be re-entered repeatedly.

### Known limitations
- No `fork`/`wait4`: one process at a time (the scheduler still has the known
  dual-scheduler problem), so the shell cannot run a program in the background and
  return to itself - `execve` replaces it.
- The userspace helpers are in the kernel image (no ELF loader on the boot path yet).
- Interrupts *during* a syscall stay disabled (the ISR stub frame handling at CPL=3
  is exercised by the timer, not by nested re-entry).

## [v0.10.1-alpha]

The ported userland libc runs. v0.10.0 shipped the port with one diagnosed blocker
(the `int 0x80` frame layout); this patch fixes it and enables the programs.

### Fixed
- **The `int 0x80` frame offsets.** `struct moos_regs` declares `rax` at +0x00, but
  the shared ISR stub pushes the GPRs as `rax, rcx, rdx, rbx, rsi, rdi, rbp,
  r8..r15`, so in memory the layout is `r15 ... rcx, rax` - i.e. rax is at **+0x70**
  (the struct's tail, vector +0x78 / error +0x80 / rip +0x88, is what really
  matches, which is why every existing handler worked). `kernel/syscall/int80.c`
  now reads the arguments and writes the result with explicit offsets
  (`FRAME_RAX/RDI/RSI/RDX`). Before this, the handler saw `nr=0` / `nr=-38` and the
  libc programs hung in ring 3 with no output.

### Verification: the ported libc runs
```
[user] running 'probe': va=0x40000000 len=10249 stack=0x4000fff0
[probe] 1 int80 write ok
[probe] 2 puts ok
[probe] 3 printf ok argc=1 argv0=user64
[probe] 4 malloc ok len=32 str=abcdefghijklmnopqrstuvwxyzabcdef
[probe] 5 pid=0 ticks=1792728129
[probe] 6 exiting with 7
[user] 'probe' exited with 7
[user] running 'hello': va=0x40000000 len=5161 stack=0x4000fff0
hello from Vimtu64 C userland
[HELLO] argc=1 argv0=user64 pid=0 ticks=1792728129
[user] 'hello' exited with 0
int80 selftest: RESULT PASS (2/2)   (14 calls served, 0 -ENOSYS)
```
That is: a C program built against the ported libc runs at CPL=3, its `printf`
reaches the console through `int 0x80` -> `write(2)`, `malloc` and `string.h`
work, and its exit code comes back to the kernel.

### Regression
self-tests 13/14, rtc 8/8, pipe 16/16, ring3test 9/9, FAT32 write+ops PASS, host
17/17, AP 1 online, RTL8139 ping, PS/2 mouse, sudo 23/23 - all unchanged.

## [v0.10.0-alpha]

Ported a real userland libc from VimtuOS and wired the machinery to run it, which
uncovered and fixed three genuine kernel bugs. The ported programs themselves are
**not runnable yet** - the `int 0x80` own-ABI gate reads the wrong call number
(diagnosed below) - so this release ships the infrastructure and the kernel fixes,
not the milestone.

### Added
- **`user/lib/`** - VimtuOS' minimal libc, ported nearly verbatim (GPL-3.0, same
  project family): `string.c`, `stdlib.c` (real `malloc`/`free` free-list arena),
  `stdio.c` (buffered `printf`/`puts`), `syscall.c`/`syscall.S` (own ABI +
  Linux-number fallbacks), `crt0.S` (entry, `main(argc,argv,envp)`, `exit`), and
  its headers (`stdio.h`, `stdlib.h`, `string.h`, `unistd.h`, `errno.h`,
  `unistd.h`, `vimtu64.h`, `sys/types.h`, `sys/stat.h`, ...).
- **`user/lib/user.ld`** - flat-blob linker script at VA `0x40000000` (VimtuOS used
  4 GiB); `.text.start` first, `.l*` sections for `-mcmodel=large`, `.image_end`
  sentinel so the `.bss` tail is part of the blob.
- **`user/apps/hello.c`** and **`user/apps/probe.c`** (staged acceptance program:
  int-0x80 write -> puts -> printf -> malloc -> getpid/ticks -> exit(7)).
- **Build integration**: `scripts/build.py` compiles the libc once, links each app
  with the flat-blob script and embeds the blob in the kernel image
  (`_binary_<name>_bin_start/end/size`), 32 KiB load-window limit enforced.
- **`kernel/syscall/int80.c`** - the Vimtu64 own-ABI gate on vector 0x80 (DPL=3):
  write/read/open/close/getpid/ticks/sleep_ms/exit forwarded onto the Linux-numbered
  syscall table; everything else answers -ENOSYS.
- **`mmu_map_user_pages()` / `mmu_prepare_user_range()`** - map a fixed user VA
  window onto PMM pages (U/S=1, W for data, NX for stack), which is what a program
  linked at an absolute address needs.
- **`user_run_blob()` / `user_run_builtin()` / shell `run`** - the general ring-3
  program launcher (the v0.9.0 runtime only knew the one embedded asm blob).

### Fixed (all three found by this work)
- **`IA32_EFER.NXE` was never set.** `mmu_map_user_pages()` marks the user stack
  non-executable with PTE bit 63; with NXE off that bit is *reserved*, so every
  stack access raised a reserved-bit #PF. Symptom: the machine stopped silently
  right after "zeroed page 7" (QEMU `-d int` showed `v=0e CR2=0x0`).
- **`el0_resume_kernel()` violated the C ABI** (SysV): it restored only `rsp` and
  returned, leaving `rbx`/`rbp`/`r12-r15` holding whatever the user program left
  in them, so the caller's locals were garbage (`console_printf("%s", name)` with
  name == -38 -> #PF inside `console_printf`, CR2=0xffffffffffffffda). It now
  saves those registers in `el0_enter_user_saved()` and pops them on the way back.
- **`sys_exit()` indexed `sched_cpu.tasks[tid]` with `tid == -1`** when the kernel
  itself is the caller (the ring-3 programs run from kmain), writing out of bounds
  and corrupting kernel state. Guarded like the fd syscalls.

### Known blocker (why the ported programs do not run yet)
`int 0x80` *works* - the asm probe program calls `getpid` through it and gets an
answer - but for the libc programs the handler reads the wrong call number
(`nr=0` / `nr=-38` in the log). The reason: `struct moos_regs` declares `rax` at
offset +0x00, while the shared ISR stub pushes `rax` first, so it lands at +0x70.
The proper fix is a dedicated `int 0x80` stub that passes `(nr, a1, a2, a3)` to C
in registers instead of through that frame. Until then `user_run_builtin()`
refuses to start the libc programs (running one hangs the machine) and says so.

### Verification
- Regression unchanged and green: self-tests 13/14, FAT32 write+ops PASS, host
  17/17, AP 1 online, RTL8139 ping, PS/2 mouse, rtc 8/8, pipe 16/16, sudo 23/23,
  `ring3test` 9/9, and the asm ring-3 program still prints
  `hello from ring 3` / `int80 getpid=3` / `uname sysname=Linux` and exits 42.
- `hello` (5161 B) and `probe` (10249 B) build and link at 0x40000000 and are
  embedded in the kernel; the loader maps them, copies them and enters ring 3
  (QEMU `-d int` shows the calls), but the own-ABI frame bug stops them there.

## [v0.9.0-alpha]

**Ring 3 runs.** The audit's biggest gap - "everything is in ring 0, there is no
userland" - is closed: a real CPL=3 program executes, makes syscalls through the
`syscall` instruction, gets kernel data copied into its own buffer, and returns
an exit code to the kernel.

### Added
- **`user/init.S`**: the first user program (334-byte flat image, no libc). It
  calls `write(1, ...)`, `uname()`, `getpid()` and `exit(42)`.
- **`kernel/syscall/ring3.c`**: the ring-3 runtime - PMM-owned user pages mapped
  with U/S=1, SSE enabled (CR4.OSFXSR|OSXMMEXCPT, CR0.EM/TS clear), GS base set up
  for the syscall entry, `el0_validate` armed, 16 KiB dedicated syscall stack.
- **`el0_enter_user_saved()` / `el0_resume_kernel()`**: enter CPL=3 and come back
  when the program exits (`sys_exit` recognises a ring-3 caller).
- **Fault isolation**: an exception taken at CPL=3 kills the user program with
  Linux's `128+signal` convention and the kernel keeps running instead of
  panicking (or, worse, re-executing the fault forever).
- **`ring3` / `ring3test`** shell commands + a boot-time run with a 9-check
  acceptance suite.

### Fixed (each of these blocked ring 3)
- **`el0_enter_user()` was broken three ways** and had never been called: it
  pushed/popped its two arguments in the wrong order (CR3 got the *context
  pointer*), every `user_context_t` offset was wrong (fs_base read from offset
  112 = rax), and it built an iret frame then executed `sysretq`. Rewritten for a
  single address space with `iretq`.
- **`syscall.S` built an incomplete frame** (only the 15 GPRs), so `ctx->cs` /
  `ctx->rip` were read from stale stack - which is why the signal-delivery hook
  never fired. It now saves a complete `user_context_t` and returns with `iretq`.
- **`IA32_EFER.SCE` was never set**, so the very first `syscall` raised #UD
  (vector 6 at user rip+0x1c). Also fixed `STAR`: the SYSCALL field held 0x10
  (kernel *data*) instead of 0x08 (kernel *code*), and the SYSRET field was wrong.
- **`fd_tables_init()` was never called**: the per-task fd table was all zeros,
  so `write(1)` did not take the stdout path and `open()` from ring 3 would always
  have failed with EMFILE.
- **`sched_cpu.current == -1`** while the kernel itself makes syscalls (the
  boot-time ring-3 run) indexed `fd_tables[-1]` out of bounds and swallowed the
  program's output. fd 1/2 are now resolved before the table lookup, and every
  fd syscall validates the task slot.
- **The ring-3 syscall ran on the launcher's stack**, overwriting the return
  address that `el0_enter_user_saved()` had saved - so `exit(60)` "returned" into
  garbage (observed: kernel #UD at rip=0x3). It has a dedicated stack now.

### Verification
- Boot log with the program running end to end:
  ```
  [ring3] entering CPL=3: entry=0x0000000000daa000 ...
  hello from ring 3
  uname sysname=Linux
  pid=0
  [ring3] the user program returned (exit=42)
  [  OK  ] ring3: first user program ran, exit=42
  ```
- `ring3test`: **9/9 PASS** (blob, user pages, SSE, FPU flags, GS base, program
  ran, exit code 42, user PTE U=1, kernel text U=0).
- Regression unchanged: self-tests 13/14, FAT32 write+ops PASS, host 17/17,
  AP 1 online, RTL8139 ping, PS/2 mouse, rtc 8/8, pipe 16/16, sudo 23/23.

### Known limitations
- The program runs with **IF=0**: a hardware interrupt taken at CPL=3 does not
  return correctly yet (the legacy same-CPL ISR stubs assume the kernel frame),
  so interrupts are re-enabled only after control is back in ring 0. The program
  needs no timer; a real ring-3 preemptive path is the next step.
- One shared page table with U/S bits rather than a per-process address space, so
  no `fork`/`execve` of a second image yet; the ELF loader (`loader.c`) is wired
  to the new entry but unused by the boot path.
- Still no libc in ring 3 (the program is hand-written assembly), and
  `dup`/`ftruncate`/`readlink`/`symlink` remain `-ENOSYS` as documented in
  v0.8.0-alpha.

## [v0.8.0-alpha]

The ring-0 half of the audit's "unfinished" list: the syscall surface a libc
needs, a real wall clock, pipes, file-management commands, and signal delivery
wired into the one return path this kernel has.

### Added
- **56 syscalls registered** (was 22). New: `stat`/`fstat`/`lstat`, `access`,
  `lseek`, `ioctl`, `dup`/`dup2`, `fsync`/`fdatasync`/`sync`, `truncate`,
  `statfs`, `chdir`/`getcwd`, `mkdir`/`rmdir`/`unlink`/`rename`, `getdents64`,
  `chmod`/`umask`, `getpid`/`gettid`/`getppid`, `getuid`/`geteuid`/`getgid`/
  `getegid`/`setuid`/`setgid`, `uname`, `gettimeofday`/`time`/`clock_gettime`
  (now the real clock)/`nanosleep`/`times`, `kill`/`wait4`/`exit_group`,
  `sysinfo`/`getrlimit`/`setrlimit`/`prctl`/`sched_getaffinity`/`getcpu`,
  `pipe`/`pipe2`. Relative paths resolve against the cwd, `stat` fills Linux's
  byte-exact 144-byte `struct stat`, and `uname` reports `sysname = "Linux"` with
  release `6.1.0-sutureos` so a libc's checks pass.
- **CMOS RTC driver** (`arch/x86_64/rtc.c`): the wall clock was faked (a constant
  1234567890); it now reads the RTC once at boot (BCD/binary, 12h/24h, century
  register) and anchors the tick counter on it. `date` prints real UTC.
- **Pipes** (`kernel/ipc/pipe.c`): pipe fds live in their own number space and
  `read`/`write`/`close` route to them; EOF when writers close, `-EPIPE` with no
  readers, short writes when full, bounded wait when empty.
- **Shell**: `rm`, `mkdir`, `rmdir`, `touch`, `cp`, `mv`, `date`, `uname`, `free`,
  and three acceptance commands `systest` / `pipetest` / `rtctest`.
- **Signal delivery**: `sig_check_pending()` is called on the syscall return path
  for user tasks (it had no call site at all), and `kill(2)` delivers into it.

### Fixed
- **ramfs `rename` was a stub** returning `VFS_EPERM`: implemented as a
  same-directory rename (what `mv a b` in one directory needs) with EEXIST/EXDEV
  checks, which is what made `systest`'s rename check fail at first.
- **RTC 12-hour decoding**: the PM flag (bit 7) was still present when the hour
  was converted, so "1 PM" became 141 instead of 13. Strip it before converting.
- RTC init decoded the year twice (BCD-decoding an already-binary year), which
  made every read look implausible and left `date` at 1970-01-01.
- `date` padding: this kernel's `snprintf` has no `0` flag, so the formatter now
  pads manually (`2026-10-22 23:04:11 UTC`, not `2026-10- 1  1: 4:11`).
- The self tests used `%-43s`, which `console_printf` prints literally (its
  formatter has no `-` flag) - replaced with plain `%s`.

### Verification
- `systest`: **39/39 PASS** (stat/lseek/read/write/fstat, mkdir/getdents64/
  unlink/rmdir, rename, chdir/getcwd, getpid/getuid/geteuid, uname, real
  clock_gettime/gettimeofday/time, pipe over the syscall path, sysinfo,
  getrlimit, sync, and the honest -ENOSYS/-ENOTTY answers).
- `pipetest`: **16/16 PASS**; `rtctest`: **8/8 PASS** (BCD/binary/12h/leap/format).
- Interactive: `date` -> `2026-10-22 23:06:17 UTC`; `mkdir /demo`, `touch`,
  `cp /etc/motd /demo/b.txt` (92 bytes), `ls` (a.txt, b.txt), `mv` (c.txt),
  `rm`, `rmdir`, `free`, `uname -a` all worked.
- Regression unchanged: self-tests 13/14, FAT32 write+ops PASS, host 17/17,
  AP 1 online, RTL8139 ping, PS/2 mouse, sudo 23/23.

### Still open (the ring-3 half, next batch)
- No userland yet: `/init` is never executed, `dynlink.c` is unreferenced, and
  `dup`/`dup2`/`ftruncate`/`readlink`/`symlink` answer `-ENOSYS` because the fd
  table has no dup API and the VFS has no symlinks. e1000 (VMware networking),
  FAT32 LFN, ext2 write and the GUI work are untouched.

## [v0.7.0-alpha]

First real privilege model: uids/gids, `/etc/passwd` + `/etc/shadow` with hashed
passwords, a `/etc/sudoers` policy parser, and a `sudo` that behaves like Linux's
sudo (ticket cache, 3 password tries, NOPASSWD, `-l/-k/-v/-u/-i`, audit log).

### Added
- **Crypto primitives** (`lib/sha256.c`, `include/stitch/sha256.h`): SHA-256
  (FIPS 180-4), HMAC-SHA256 (RFC 2104) and PBKDF2-HMAC-SHA256 (RFC 2898), all
  covered by known-answer self-tests that run at every boot - a broken hash would
  otherwise let every password through.
- **User database** (`kernel/security/userdb.c`): parses `/etc/passwd`,
  `/etc/group` (Linux formats) and `/etc/shadow`, seeding them on first boot with
  `root`/`root` (uid 0) and `user`/`user` (uid 1000, member of `wheel`). Password
  hashes are salted PBKDF2-HMAC-SHA256 stored as
  `$sutureos$pbkdf2-sha256$<iters>$<salt>$<hash>` - deliberately not Linux's
  crypt format, because it is not one.
- **Credentials** (`kernel/security/creds.c`): per-task uid/euid/suid/gid/egid
  (Linux shape). `cred_set_euid(0)` fails for a normal user on purpose; only
  `cred_escalate_root()` - called by sudo after authentication - may raise it.
- **sudo** (`kernel/security/sudo.c`, `sudoers.c`): Linux sudo's contract -
  `sudoers` syntax (`user`/`%group HOST=(RUNAS:GROUP) [NOPASSWD:] CMND`, `Defaults
  timestamp_timeout`/`passwd_tries`), timestamp ticket cache (15 min, `-k`
  invalidates, `-v` refreshes), 3 attempts with `Sorry, try again.` and
  `sudo: 3 incorrect password attempts`, both denial messages, `sudo -l`,
  `sudo -u`, `sudo -i`, and one audit line per invocation in `/var/log/sudo.log`.
- **Shell**: `su [user]`, `whoami`, `id`, `passwd [user]` (two hidden prompts),
  `sudotest` (23-check acceptance), a prompt that shows the identity
  (`sutureos# ` as root, `user@sutureos$ ` otherwise) and real permission checks
  on the privileged commands `mount`/`umount`/`shutdown`/`reboot`/`kill`.

### Fixed
- **ramfs files were capped at 64 bytes** (`uint32_t data[16]`): `/etc/motd` had
  silently been truncated and `/etc/passwd` could not exist at all. Nodes now
  carry `XJ_VFS_MAX_FILE_SIZE` (2048) bytes - this is what made the user database
  possible.
- **Dropped keystrokes at typing speed**: the 8042 has a one-byte output buffer
  and the shell read it once per idle iteration, *after* draining the SMP work
  queue and the NIC ring, so fast input was lost (typing `su user` arrived as
  `user`). A 64-byte input ring is filled with a burst of polls and drained by
  `console_getc()`; the keyboard is now serviced before the other idle work.
- PBKDF2 iterations lowered from 10000 to 2000: 10k cost ~1 s per verification,
  which stalled the shell (and made the password prompt feel broken).

### Verification
- `sudotest`: **23/23 PASS** (hashes, accounts, shadow round-trip, policy,
  ticket, credentials, audit log).
- Interactive QEMU transcript: `su user` -> `user@sutureos$`; `mount ...` denied
  with `permission denied (are you root? try 'sudo mount')`; `sudo -l` lists the
  policy after the password; `sudo mount fat32 0 /mnt/sudomnt` actually mounts and
  logs `RUN user=user runas=root cmd=mount ...`; a second `sudo` runs without a
  password (ticket); `sudo -k` forces the prompt again; 3 wrong passwords produce
  `sudo: 3 incorrect password attempts`; `su root` restores `sutureos#`.
- Regression: self-tests 13/14, FAT32 write+ops PASS, host verifier 17/17,
  AP online, RTL8139 ping, PS/2 mouse - unchanged.

### Known limitations (stated, not hidden)
- This is a **policy layer, not a security boundary**: every task still runs in
  ring 0 and the shell is the only user, so kernel code can bypass sudo. It
  becomes a real boundary when ring-3 userland lands; the cred API already has
  Linux's uid/euid/suid shape so the switch is mechanical.
- Linux sudo itself is a setuid userspace program built on PAM/crypt(3)/termios.
  None of those exist here (no libc in the image, 22 syscalls), so the behaviour
  was ported rather than the binary; password hashing is PBKDF2-HMAC-SHA256
  instead of yescrypt, and there is no secure RNG yet (salts mix the TSC, a
  counter and the user name).
- No `sudoedit`, no PAM modules, no LDAP, no `Defaults` beyond the two above, no
  per-command argument granularity beyond the command name.

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
