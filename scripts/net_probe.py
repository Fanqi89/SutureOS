#!/usr/bin/env python3
"""Host-side verifier for the SutureOS network bring-up.

Parses a QEMU serial log (and optionally a host-side packet capture) and checks
the acceptance criteria for the RTL8139 + ARP/IPv4/ICMP/UDP bring-up:

  * the two mandatory bring-up lines, with a plausible I/O base, IRQ and MAC
  * the polled mode announcement
  * the exact self-test outcome line (reply or TIMEOUT)
  * the statistics block, with counters that actually advanced

Usage:
    py -3 scripts/net_probe.py build/serial.log
    py -3 scripts/net_probe.py build/serial.log --capture build/net.pcap

Exit status is 0 when every required check passes, 1 otherwise, so the script
can gate an automated run.
"""
import argparse
import re
import struct
import sys

# ---- bring-up lines -------------------------------------------------------
RE_FOUND = re.compile(
    r"^\[net\] rtl8139 found at 0x(?P<io>[0-9a-fA-F]+), irq=(?P<irq>\d+), "
    r"mac=(?P<mac>(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2})\s*$")
RE_IPV4 = re.compile(
    r"^\[net\] IPv4 (?P<ip>\d+\.\d+\.\d+\.\d+)/(?P<prefix>\d+) "
    r"gw (?P<gw>\d+\.\d+\.\d+\.\d+) \(mode=(?P<mode>poll|irq)\)\s*$")
RE_FAILED = re.compile(r"^\[net\] init failed: (?P<why>.+)$")
RE_SELFTEST_OK = re.compile(r"^\[net\] selftest ping 10\.0\.2\.2 -> reply in (?P<us>\d+) us\s*$")
RE_SELFTEST_TO = re.compile(r"^\[net\] selftest ping TIMEOUT\s*$")
RE_FRAMES = re.compile(r"^\[net\] frames rx=(?P<rx>\d+) tx=(?P<tx>\d+) drops=(?P<drops>\d+)")
RE_ARP = re.compile(r"^\[net\] arp rx=(?P<rx>\d+) tx=(?P<tx>\d+) entries=(?P<e>\d+)")
RE_IP = re.compile(r"^\[net\] ip rx=(?P<rx>\d+) tx=(?P<tx>\d+) fwd=(?P<fwd>\d+) badsum=(?P<bad>\d+)")
RE_ICMP = re.compile(r"^\[net\] icmp rx=(?P<rx>\d+) tx=(?P<tx>\d+)")
RE_UDP = re.compile(r"^\[net\] udp rx=(?P<rx>\d+) tx=(?P<tx>\d+) dropped=(?P<d>\d+)")
RE_LAST = re.compile(
    r"^\[net\] last rx (?P<src>\d+\.\d+\.\d+\.\d+) -> (?P<dst>\d+\.\d+\.\d+\.\d+) "
    r"proto=(?P<proto>\d+) len=(?P<len>\d+) ttl=(?P<ttl>\d+)")
RE_MODE = re.compile(r"^\[net\] mode=poll poll_calls=(?P<calls>\d+)")
RE_NIC = re.compile(
    r"^\[net\] nic io=0x(?P<io>[0-9a-fA-F]+) irq=(?P<irq>\d+) present=(?P<present>\d+) "
    r"isr=0x(?P<isr>[0-9a-fA-F]+) ring_drops=(?P<drops>\d+)")
RE_SELFTEST_UDP = re.compile(r"^\[net\] selftest udp tx dns query (?P<n>\d+) bytes")
RE_SELFTEST_FRAMES = re.compile(
    r"^\[net\] selftest frames rx=(?P<rx>\d+) tx=(?P<tx>\d+) arp_entries=(?P<e>\d+)")

PROTO_NAMES = {1: "ICMP", 6: "TCP", 17: "UDP"}


class Report:
    def __init__(self):
        self.checks = []

    def check(self, ok, label, detail=""):
        self.checks.append((bool(ok), label, detail))
        mark = "PASS" if ok else "FAIL"
        line = "  [%s] %s" % (mark, label)
        if detail:
            line += "  (%s)" % detail
        print(line)

    @property
    def failures(self):
        return sum(1 for ok, _, _ in self.checks if not ok)


def parse_log(path):
    with open(path, "r", errors="replace") as fh:
        return [ln.rstrip("\r\n") for ln in fh]


def check_serial(lines, rep):
    found = [m for m in (RE_FOUND.match(l) for l in lines) if m]
    ipv4 = [m for m in (RE_IPV4.match(l) for l in lines) if m]
    failed = [m for m in (RE_FAILED.match(l) for l in lines) if m]

    if not found:
        # Either the NIC was never initialised or the machine has no 8139.
        if failed:
            rep.check(False, "net_init() succeeded",
                      "log says: %s" % failed[0].group("why"))
        else:
            rep.check(False, "net_init() ran", "no '[net] rtl8139 found at' line")
        return False

    m = found[0]
    io_base = int(m.group("io"), 16)
    irq = int(m.group("irq"))
    mac = m.group("mac")
    rep.check(0x100 <= io_base <= 0xFF00, "I/O base is a valid port range",
              "0x%x" % io_base)
    rep.check(1 <= irq <= 15, "IRQ line is in 1..15", "irq=%d" % irq)
    rep.check(mac.lower() != "00:00:00:00:00:00" and mac.lower() != "ff:ff:ff:ff:ff:ff",
              "MAC is a real address", mac)
    rep.check(not failed, "no '[net] init failed' line", 
              failed[0].group("why") if failed else "")

    if not ipv4:
        rep.check(False, "IPv4 configuration line present",
                  "expected '[net] IPv4 10.0.2.15/24 gw 10.0.2.2 (mode=poll)'")
    else:
        g = ipv4[0].groupdict()
        rep.check(g["ip"] == "10.0.2.15", "address is 10.0.2.15", g["ip"])
        rep.check(g["prefix"] == "24", "prefix is /24", g["prefix"])
        rep.check(g["gw"] == "10.0.2.2", "gateway is 10.0.2.2", g["gw"])
        rep.check(g["mode"] == "poll", "mode recorded as poll", g["mode"])

    ok_lines = [m for m in (RE_SELFTEST_OK.match(l) for l in lines) if m]
    to_lines = [m for m in (RE_SELFTEST_TO.match(l) for l in lines) if m]
    if ok_lines:
        us = int(ok_lines[0].group("us"))
        rep.check(True, "self-test ping to 10.0.2.2 got a reply",
                  "%d us" % us)
    elif to_lines:
        rep.check(False, "self-test ping to 10.0.2.2 got a reply",
                  "log shows '[net] selftest ping TIMEOUT'")
    else:
        rep.check(False, "self-test ping ran",
                  "no '[net] selftest ping' line (call net_selftest())")

    fr = [m for m in (RE_FRAMES.match(l) for l in lines) if m]
    if fr:
        g = fr[-1].groupdict()
        rep.check(int(g["rx"]) > 0, "frames received > 0", "rx=%s tx=%s" % (g["rx"], g["tx"]))
        rep.check(int(g["tx"]) > 0, "frames transmitted > 0",
                  "rx=%s tx=%s" % (g["rx"], g["tx"]))
        rep.check(int(g["drops"]) == 0, "no stack-level drops", "drops=%s" % g["drops"])
    else:
        rep.check(False, "net_dump_stats() ran", "no '[net] frames rx=' line")

    ar = [m for m in (RE_ARP.match(l) for l in lines) if m]
    if ar:
        g = ar[-1].groupdict()
        rep.check(int(g["rx"]) >= 1 and int(g["tx"]) >= 1,
                  "ARP exchange happened", "rx=%s tx=%s entries=%s"
                  % (g["rx"], g["tx"], g["e"]))
        rep.check(int(g["e"]) >= 1, "ARP cache has at least one entry",
                  "entries=%s" % g["e"])
    else:
        rep.check(False, "ARP counters present", "no '[net] arp rx=' line")

    ipc = [m for m in (RE_IP.match(l) for l in lines) if m]
    if ipc:
        g = ipc[-1].groupdict()
        rep.check(int(g["rx"]) >= 1 and int(g["tx"]) >= 1,
                  "IPv4 traffic in both directions",
                  "rx=%s tx=%s fwd=%s" % (g["rx"], g["tx"], g["fwd"]))
        rep.check(int(g["bad"]) == 0, "no bad IPv4/ICMP/UDP checksums",
                  "badsum=%s" % g["bad"])
    else:
        rep.check(False, "IP counters present", "no '[net] ip rx=' line")

    ic = [m for m in (RE_ICMP.match(l) for l in lines) if m]
    if ic:
        g = ic[-1].groupdict()
        rep.check(int(g["rx"]) >= 1 and int(g["tx"]) >= 1,
                  "ICMP echo in both directions",
                  "rx=%s tx=%s" % (g["rx"], g["tx"]))
    else:
        rep.check(False, "ICMP counters present", "no '[net] icmp rx=' line")

    ud = [m for m in (RE_UDP.match(l) for l in lines) if m]
    if ud:
        g = ud[-1].groupdict()
        rep.check(True, "UDP counters present",
                  "rx=%s tx=%s dropped=%s" % (g["rx"], g["tx"], g["d"]))
    else:
        rep.check(False, "UDP counters present", "no '[net] udp rx=' line")

    la = [m for m in (RE_LAST.match(l) for l in lines) if m]
    if la:
        g = la[-1].groupdict()
        name = PROTO_NAMES.get(int(g["proto"]), "proto=%s" % g["proto"])
        rep.check(True, "last received packet summarised",
                  "%s -> %s %s len=%s ttl=%s"
                  % (g["src"], g["dst"], name, g["len"], g["ttl"]))
    else:
        rep.check(False, "last received packet summarised", "no '[net] last rx' line")

    md = [m for m in (RE_MODE.match(l) for l in lines) if m]
    if md:
        rep.check(int(md[-1].group("calls")) > 0, "poll loop ran",
                  "poll_calls=%s" % md[-1].group("calls"))
    else:
        rep.check(False, "poll-mode line present", "no '[net] mode=poll' line")

    udp_tx = [m for m in (RE_SELFTEST_UDP.match(l) for l in lines) if m]
    sf = [m for m in (RE_SELFTEST_FRAMES.match(l) for l in lines) if m]
    if udp_tx:
        rep.check(True, "UDP datagram left the guest",
                  "%s bytes -> 10.0.2.3:53" % udp_tx[-1].group("n"))
    if sf:
        g = sf[-1].groupdict()
        rep.check(int(g["rx"]) >= 1 and int(g["tx"]) >= 1,
                  "self-test saw frames on the wire",
                  "rx=%s tx=%s arp_entries=%s" % (g["rx"], g["tx"], g["e"]))

    return not rep.failures


# ---- optional pcap cross-check -------------------------------------------
def read_pcap(path):
    """Yield (src_mac, dst_mac, ethertype, payload) for classic pcap files."""
    with open(path, "rb") as fh:
        magic = fh.read(4)
        if magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
            endian, nano = "<", magic == b"\x4d\x3c\xb2\xa1"
        elif magic in (b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
            endian, nano = ">", magic == b"\xa1\xb2\x3c\x4d"
        else:
            raise ValueError("not a classic pcap file")
        _maj, _min, _tz, _sig, _snap, linktype = struct.unpack(endian + "HHiIII", fh.read(20))
        if linktype != 1:
            raise ValueError("pcap link type %d is not ethernet" % linktype)
        while True:
            hdr = fh.read(16)
            if len(hdr) < 16:
                return
            _ts, _tus, caplen, _orig = struct.unpack(endian + "IIII", hdr)
            data = fh.read(caplen)
            if len(data) < 14:
                continue
            yield (data[6:12].hex(":"), data[0:6].hex(":"),
                   struct.unpack("!H", data[12:14])[0], data[14:])


def check_capture(path, expected_mac, rep):
    try:
        frames = list(read_pcap(path))
    except Exception as exc:                     # noqa: BLE001 - report and continue
        rep.check(False, "host capture readable", str(exc))
        return
    rep.check(len(frames) > 0, "host capture has frames", "%d frames" % len(frames))

    arp = [f for f in frames if f[2] == 0x0806]
    ip4 = [f for f in frames if f[2] == 0x0800]
    icmp = [f for f in ip4 if f[3][9] == 1]
    udp = [f for f in ip4 if f[3][9] == 17]
    rep.check(len(arp) > 0, "ARP visible in the capture", "%d frames" % len(arp))
    rep.check(len(icmp) > 0, "ICMP visible in the capture", "%d frames" % len(icmp))
    if expected_mac:
        ours = [f for f in frames if f[0] == expected_mac.lower()]
        rep.check(len(ours) > 0, "guest MAC observed as sender",
                  "%s in %d frames" % (expected_mac, len(ours)))
    if udp:
        rep.check(True, "UDP visible in the capture", "%d frames" % len(udp))


def main():
    ap = argparse.ArgumentParser(description="Verify the SutureOS network bring-up from a serial log.")
    ap.add_argument("serial_log", help="path to the QEMU -serial file: log")
    ap.add_argument("--capture", help="optional host-side pcap of the same run")
    args = ap.parse_args()

    lines = parse_log(args.serial_log)
    net_lines = [l for l in lines if l.startswith("[net]")]
    print("serial log: %s (%d lines, %d net lines)" %
          (args.serial_log, len(lines), len(net_lines)))
    print()

    rep = Report()
    check_serial(lines, rep)

    mac = None
    for l in lines:
        m = RE_FOUND.match(l)
        if m:
            mac = m.group("mac")
            break
    if args.capture:
        print()
        check_capture(args.capture, mac, rep)

    total = len(rep.checks)
    print()
    print("%s: %d checks, %d failed" %
          ("FAILED" if rep.failures else "ALL PASS", total, rep.failures))
    return 1 if rep.failures else 0


if __name__ == "__main__":
    sys.exit(main())
