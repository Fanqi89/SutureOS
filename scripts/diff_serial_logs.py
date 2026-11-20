#!/usr/bin/env python3
"""Compare two SutureOS serial logs (regression evidence for a driver port).

Why normalization is needed: two boots of the same ISO are never byte-identical -
physical addresses, ticks, LAPIC calibration, e1000 counters and ping timings all
change per boot.  Comparing raw lines therefore reports the whole log as
"changed" and proves nothing.

So each line is normalised first:
  * hex literals           0x1f2a3b4          -> 0x#
  * decimal runs >= 3 digits 24576, 63172994   -> #
  * "<date> <time>" stamps  2026-11-20 09:30:31 -> #TIME#
  * lines that are known to be timing/queue dependent (ping selftest outcome,
    e1000 idle register dump) are dropped entirely
Small numbers (1-2 digits) are kept on purpose, so a selftest count going
16/16 -> 15/15 or "checks=23 ok=22" would still show up as a difference.

The comparison then requires every stable baseline line to appear, in order, in
the new log, and prints exactly which stable lines were added or removed.

Usage:
  py -3 scripts/diff_serial_logs.py build/serial_baseline_nodisk.log \
                                      build/serial_vimtufs2_nodisk.log
"""
import re
import sys

HEX = re.compile(r"0x[0-9a-fA-F]+")
BIGNUM = re.compile(r"\b\d{3,}\b")
STAMP = re.compile(r"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d")
# Lines whose content depends on boot-time timing / device queue state.
VOLATILE_MARKERS = (
    "selftest ping TIMEOUT",
    "e1000] idle:",
    "e1000 diag:",
    "[net] selftest frames",
    "[net] last rx",
)


def normalise(line):
    line = STAMP.sub("#TIME#", line)
    line = HEX.sub("0x#", line)
    line = BIGNUM.sub("#", line)
    return line


def is_volatile(line):
    return any(m in line for m in VOLATILE_MARKERS)


def load(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return [ln.rstrip("\r\n") for ln in fh]


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) < 2:
        print("usage: diff_serial_logs.py <baseline> <current>")
        return 2
    base_raw = load(argv[0])
    cur_raw = load(argv[1])
    base = [normalise(ln) for ln in base_raw if not is_volatile(ln)]
    cur = [normalise(ln) for ln in cur_raw if not is_volatile(ln)]
    bset, cset = set(base), set(cur)
    removed = [ln for ln in base if ln not in cset]
    added = [ln for ln in cur if ln not in bset]

    print("[diff] baseline: %d lines (%d stable) from %s"
          % (len(base_raw), len(base), argv[0]))
    print("[diff] current : %d lines (%d stable) from %s"
          % (len(cur_raw), len(cur), argv[1]))
    print("[diff] volatile lines dropped: baseline=%d current=%d"
          % (len(base_raw) - len(base), len(cur_raw) - len(cur)))
    print("[diff] added stable lines = %d" % len(added))
    for ln in added:
        print("   + %s" % ln)
    print("[diff] removed stable lines = %d" % len(removed))
    for ln in removed:
        print("   - %s" % ln)

    # order-preserving subsequence: the new log must still contain every
    # baseline line, in the baseline order (only added lines may be skipped)
    i = 0
    for ln in cur:
        if i < len(base) and ln == base[i]:
            i += 1
    ordered = (i == len(base))
    print("[diff] every stable baseline line still present in order:", ordered)
    ok = ordered and not removed
    print("[diff] RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
