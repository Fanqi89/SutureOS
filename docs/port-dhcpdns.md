# DHCP client + DNS resolver — port report (副本 `C:\sutureos_ports\dhcpdns`)

Network-domain gap: the stack had IPv4/ARP/ICMP/UDP/TCP and a *static* slirp
default (`10.0.2.15/24`, gw `10.0.2.2`, DNS `10.0.2.3`), but no automatic
addressing and no name resolution. This report records what already existed, what
was missing, what changed, and the real QEMU evidence.

## 1. What already existed (not a stub)

`kernel/net/dhcp.c/.h` and `kernel/net/dns.c/.h` are complete drivers, not stubs:

| file | content | verdict |
|---|---|---|
| `dhcp.c/.h` | BOOTP/DHCP packet builder (options 53/61/55/50/54), reply parser (yiaddr@16, option 1/3/6/51/54), `dhcp_client_acquire()`, silent self-test | complete protocol layer |
| `dns.c/.h` | A/IN query builder (QNAME encoding, RD flag), answer parser with compression-pointer skipping + loop guard, `dns_resolve()`, silent self-test | complete protocol layer |
| `net.c/.h` | `net_configure_ip()`, `net_set_dns_ip()`, `net_set_tx_src_ip()/net_clear_tx_src_ip()`, broadcast RX acceptance, UDP checksum over the datagram's own destination | hooks the drivers need |
| `shell.c` | `dhcp`, `nslookup` commands | present |
| `syscalls2.c` | `dns_selftest()` / `dhcp_selftest()` in the boot self-test | present, silent on success |

## 2. What was actually missing (and is fixed here)

1. **No `dhcp_acquire(uint32_t timeout_ticks)`** — only `dhcp_client_acquire(lease, ms)`.
2. **DORA emitted no log lines at all**, so nothing was assertable from serial.
3. **The timeout argument was a lie**: the wait used a blocking `net_recvfrom()`
   (fixed 3 s internal stall) × 4 retries → worst case 12 s per phase, and the
   caller's budget was ignored.
4. **DNS emitted no log lines** (no query/answer/nxdomain/timeout).
5. **No `dns <name>` shell command** (only the chatty `nslookup`).
6. **A real bug found while testing**: `net_recvfrom(..., MSG_DONTWAIT, ...)`
   returns `-EAGAIN` *before* `net_dgram_recv()` pumps the RX ring, so a
   `MSG_DONTWAIT` polling loop never receives anything. Both new loops now call
   `net_poll()` themselves. Without this, DISCOVER/OFFER and the DNS query
   always timed out even though `ping` worked.
7. `wget` accepted only IPv4 literals (optional bonus, done): a hostname is now
   resolved through the DHCP-provided resolver; a failure prints
   `wget: cannot resolve host '<name>'` instead of blaming the URL syntax.

## 3. Files changed (this port copy only)

* `kernel/net/dhcp.h` — added `int dhcp_acquire(uint32_t timeout_ticks);`
* `kernel/net/dhcp.c`
  * `DHCP_TICK_HZ` (100 Hz, same constant family as `sudo.c`), `DHCP_MIN_BUDGET_US`;
  * `dhcp_step()` rewritten: bounded `net_poll()` + `MSG_DONTWAIT` wait, one
    send per phase (instead of 4 blocking 3 s waits);
  * `dhcp_client_acquire()` — logs `[dhcp] DISCOVER/OFFER/REQUEST/ACK`, splits the
    caller's budget between DISCOVER/OFFER and REQUEST/ACK;
  * `dhcp_acquire()` — tick-based spec entry point (0 = 4 s default);
  * `dhcp_ip_str()` helper.
* `kernel/net/dns.c`
  * `dns_rcode()` helper + `DNS_RCODE_NXDOMAIN`;
  * `dns_resolve()` — `net_poll()` + `MSG_DONTWAIT` per-attempt wait that honours
    `timeout_ms`, logs `[dns] query .. id=0x..`, `[dns] answer .. -> ip (1 A record)`,
    `[dns] nxdomain ..`, `[dns] timeout ..`.
* `kernel/shell/shell.c`
  * `dhcp` uses `dhcp_acquire(400)` and prints the four-tuple; no-NIC message is
    `dhcp: no NIC / network not ready (...)`.
  * new `dns <name>` command (concise A-record lookup; non-zero on failure).
  * `wget`: `wget_parse_url()` falls back to `dns_resolve()` (returns `-2` =
    "name did not resolve"), caller prints the honest message, usage text updated.
* `kernel/shell/shell.h` — `shell_cmd_dns` prototype.
* `scripts/dhcpdns_acceptance.py` — new acceptance (see §5).
* `scripts/build_iso.py` — **port-copy fix only**: `msys_path` was still hardcoded
  to `/c/sutureos_iso_temp/iso_root` while `temp_dir` had been renamed to
  `C:/sutureos_iso_temp_dhcpdns`, so xorriso aborted with "Cannot determine
  attributes of source file". It is now derived from `temp_iso_root`.

No changes to `kernel/gui/**`, `kernel/term/**`, `fbcon`, `boot/**`,
`include/sutureos/version.h`, and no new boot self-test item (`st_*` untouched).

## 4. Integration anchors for the main repo

Required from `net.c/.h` (present in this copy; verify in main):
`net_ready`, `net_mac`, `net_poll`, `net_now_us`, `net_ip_to_str`,
`net_configure_ip`, `net_set_dns_ip`, `net_set_tx_src_ip`, `net_clear_tx_src_ip`,
`net_ip/net_gw/net_mask/net_dns`, `extern uint32_t net_dns_ip`,
`NET_IP_BROADCAST`, and the datagram socket ABI
(`net_socket/net_bind/net_sendto/net_recvfrom/net_close`, `MSG_DONTWAIT`).

RX side requirements the drivers depend on:
* `net_ip_is_ours()` must accept `255.255.255.255` (DHCP replies are broadcast),
* `udp_rx()` must validate the UDP checksum against the datagram's own
  destination IP, not `net_local_ip`.

Shell: add `#include "dhcp.h"` / `#include "dns.h"`, the three command-table rows
(`dhcp`, `dns`, `nslookup`) and the `shell.h` prototypes (append only).

Boot self-test: `syscalls2.c` calls `dns_selftest()` / `dhcp_selftest()`; both are
silent on success so the boot log and the self-test count (16/16) do not change.

## 5. Acceptance — real QEMU, real keys

`py -3 scripts/dhcpdns_acceptance.py` → **checks=17 ok=17 fail=0 PASS**

QEMU (phase A): `-cdrom build/sutureos.iso -boot d
-drive id=adisk,file=build/fat32_ata.img,format=raw,if=ide
-netdev user,id=n0 -device rtl8139,netdev=n0 -smp 2 -no-reboot`
Phase B: same ISO with `-nic none` (QEMU's pc machine otherwise adds a default
e1000 and there is no way to boot "without a NIC").

Real serial transcript (phase A):

```
sutureos# ping 10.0.2.2
--- 10.0.2.2: 3/3 replies ---
sutureos# dhcp
DHCP: sending DISCOVER to 255.255.255.255:67 ...
[dhcp] DISCOVER xid=0x007de661
[dhcp] OFFER 10.0.2.15 from 10.0.2.2
[dhcp] REQUEST ip=10.0.2.15 server=10.0.2.2
[dhcp] ACK ip=10.0.2.15 mask=255.255.255.0 gw=10.0.2.2 dns=10.0.2.3 lease=86400s
dhcp: ip 10.0.2.15 mask 255.255.255.0 gateway 10.0.2.2 dns 10.0.2.3
sutureos# dns example.com
[dns] query example.com id=0xd757
[dns] answer example.com -> 104.20.23.154 (1 A record)
dns: example.com -> 104.20.23.154
sutureos# dns google.com
[dns] query google.com id=0xc355
[dns] answer google.com -> 142.250.73.78 (1 A record)
dns: google.com -> 142.250.73.78
sutureos# wget http://nosuchhost.invalid/x
[dns] query nosuchhost.invalid id=0x1b94
[dns] nxdomain nosuchhost.invalid
wget: cannot resolve host 'nosuchhost.invalid' (run dhcp first if no DNS server is configured)
sutureos# ping 10.0.2.2
--- 10.0.2.2: 3/3 replies ---
```

Phase B (no NIC):

```
[stitch] selftests: 16/16 passed
sutureos# dhcp
dhcp: no NIC / network not ready (net_init found no RTL8139 or e1000)
sutureos#
```

Assertions (17): boot log gains no `[dhcp]`/`[dns]` lines before the prompt;
static default config still pings before `dhcp`; DISCOVER xid; `OFFER 10.0.2.15
from 10.0.2.2`; `ACK ip/mask/gw/dns/lease` exactly `10.0.2.15 /
255.255.255.0 / 10.0.2.2 / 10.0.2.3 / 86400s`; shell four-tuple; `[dns] query`
id; a valid IPv4 answer; the resolver address came from DHCP (`dns=10.0.2.3`);
wget hostname path (honest NXDOMAIN); ping still 3/3 after the lease; self-tests
16/16; no PANIC; and the four no-NIC checks.

Regression while doing this: `scripts/wget_acceptance.py` **PASS (8/8)** — the
IPv4-literal download path, the host-side byte comparison of `DL.BIN` (sha256
a870a52a6121f125 == a870a52a6121f125) and `HELLO2.TXT` are unchanged.

## 6. Known limits

* No lease renewal (no T1/T2 timer), no `RELEASE`/`DECLINE`, no hostname option,
  no relay (`giaddr`), one transaction at a time.
* DHCP parses only mask/router/DNS/lease/server-id options.
* DNS: A records only — no AAAA/CNAME chasing, no EDNS0, no TCP fallback, no cache,
  one outstanding query; the log says `(1 A record)` because the parser returns
  the first A record it finds, not because every answer has exactly one.
* Timeouts are wall-clock (`net_now_us()`), not preemption: a lookup blocks the
  shell for up to its budget (1.5 s/attempt for `dns`, split 2 s per phase for
  `dhcp`).
* `dhcp` is on demand only; boot keeps the static slirp defaults — by design, so
  the frozen boot presentation and 16/16 self-tests do not move.
* `wget` hostname→HTTP success was **not** proven end-to-end: QEMU slirp cannot
  map an arbitrary name onto the host loopback, and the host's internet path is
  not relied on. Proven: name → resolver → honest failure path, and the
  IP-literal download regression above.
* `wget` still has the old limits (HTTP/1.0 GET only, no HTTPS/redirect, 2 KiB
  headers, 5 s timeouts).
* Host-side flake, not a kernel change: a TCG boot can stall after the FAT32
  self-tests (observed twice, same `kernel.elf` that passed minutes earlier), so
  the acceptance retries the boot step and prints the retry. Do not run two
  acceptances concurrently on this 2-core host.
* `fat32_ata.img` is mutated by every boot (FAT32 self-tests) and further by
  `wget_acceptance.py`; regenerate it (`py -3 scripts/mkfat32_testimg.py` then copy
  `build/fat32.img` → `build/fat32_ata.img`) before a clean run.

## 7. Sources / licensing

Self-written. Protocol references only: **RFC 2131 / RFC 2132** (DHCP) and
**RFC 1035** (DNS). No third-party code was copied; the drivers reuse SutureOS's
own `net.c` socket ABI and `lib/kprintf.c`. Symbol prefixes: `dhcp_*` / `dns_*`.
