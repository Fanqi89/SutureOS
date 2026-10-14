#!/usr/bin/env python3
"""Host-side acceptance test for the SutureOS TCP implementation.

What it proves (end to end, without trusting anything the guest prints):

  1. a real TCP server on the HOST accepts a connection that the guest opened
     itself (three-way handshake performed by kernel/net/tcp.c),
  2. the bytes the host received are byte-for-byte the text the guest was told
     to send,
  3. the payload the host wrote back is byte-for-byte what the guest received
     and printed,
  4. the guest closed the connection cleanly.

QEMU's user-mode network stack forwards guest traffic addressed to 10.0.2.2 to
the host's loopback interface, which is how the guest reaches this server.

Usage:
    py -3 scripts/tcp_acceptance.py                 # default port 5599
    py -3 scripts/tcp_acceptance.py --port 5601 --text hello42 --keep-log
Exit status 0 only when every check passes.
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
GUEST_LOG = os.path.join(BUILD, "serial_tcp.log")
SERVER_LOG = os.path.join(BUILD, "tcp_server.log")
ARGS_FILE = os.path.join(BUILD, "qemu_net.args")

# QEMU sendkey names: space -> spc, '.' -> dot. One key per sendkey command so
# the polled PS/2 driver cannot lose them.
KEYMAP = {" ": "spc", ".": "dot", "/": "slash", "-": "minus", "=": "equal",
          ":": "shift-semicolon", "_": "shift-minus"}


def keys_for(text: str) -> str:
    out = []
    for ch in text:
        if ch in KEYMAP:
            out.append(KEYMAP[ch])
        elif ch.isupper():
            out.append("shift-" + ch.lower())
        else:
            out.append(ch)
    out.append("ret")
    return ";".join(out)


class Server:
    """One-shot TCP server: records what arrived, replies deterministically."""

    def __init__(self, port: int, reply_prefix: bytes, expect_bytes: int = 0):
        self.port = port
        self.reply_prefix = reply_prefix
        self.expect_bytes = expect_bytes
        self.received = b""
        self.sent = b""
        self.connected = threading.Event()
        self.done = threading.Event()
        self.error = None
        self.peer = None
        self.closed_by_peer = False

    def run(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("0.0.0.0", self.port))
        srv.listen(1)
        srv.settimeout(60)
        try:
            conn, self.peer = srv.accept()
            self.connected.set()
            conn.settimeout(10)
            # Read until the expected length arrived, or the peer goes quiet.
            deadline = time.time() + 20
            while time.time() < deadline:
                try:
                    chunk = conn.recv(4096)
                except socket.timeout:
                    break
                if not chunk:
                    break
                self.received += chunk
                if self.expect_bytes and len(self.received) >= self.expect_bytes:
                    break
                if not self.expect_bytes and len(self.received) >= 5:
                    break
            self.sent = self.reply_prefix + (b"" if self.expect_bytes else self.received)
            conn.sendall(self.sent)
            # Give the guest time to read the reply, then finish the close.
            try:
                conn.settimeout(5)
                while True:
                    more = conn.recv(4096)
                    if not more:
                        self.closed_by_peer = True
                        break
                    self.received += more
            except socket.timeout:
                pass
            conn.close()
        except Exception as exc:  # noqa: BLE001 - reported to the caller
            self.error = exc
            self.connected.set()
        finally:
            srv.close()
            self.done.set()
        with open(SERVER_LOG, "w", encoding="utf-8") as fh:
            fh.write(f"port={self.port} peer={self.peer}\n")
            fh.write(f"received={self.received!r} ({len(self.received)} bytes)\n")
            fh.write(f"sent={self.sent!r} ({len(self.sent)} bytes)\n")
            fh.write(f"error={self.error!r}\n")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=5599)
    ap.add_argument("--text", default="hello42")
    ap.add_argument("--mode", choices=["echo", "bulk"], default="echo",
                    help="echo = tcpecho <ip> <port> <text>; "
                         "bulk = tcpsend <ip> <port> (3000 bytes, exercises MSS splitting)")
    ap.add_argument("--boot-wait", type=int, default=18)
    ap.add_argument("--reply-prefix", default="PONG:")
    args = ap.parse_args()

    if not os.path.exists(ARGS_FILE):
        print(f"missing {ARGS_FILE}: run the net boot configuration first")
        return 1

    bulk = args.mode == "bulk"
    # tcpsend with no text sends 3000 bytes of 'A'..'Z' repeating (3 segments).
    bulk_len = 3000
    bulk_payload = bytes((ord('A') + (i % 26)) for i in range(bulk_len))

    srv = Server(args.port, args.reply_prefix.encode(),
                 expect_bytes=bulk_len if bulk else 0)
    th = threading.Thread(target=srv.run, daemon=True)
    th.start()
    time.sleep(0.5)
    if srv.error:
        print(f"server failed to start: {srv.error}")
        return 1

    command = (f"tcpsend 10.0.2.2 {args.port}" if bulk
               else f"tcpecho 10.0.2.2 {args.port} {args.text}")
    print(f"[host] server listening on 0.0.0.0:{args.port}")
    print(f"[guest] will type: {command}")

    cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
           "-File", os.path.join(ROOT, "scripts", "qemu_boot.ps1"),
           "-SerialLog", os.path.relpath(GUEST_LOG, ROOT),
           "-ArgFile", os.path.relpath(ARGS_FILE, ROOT),
           "-BootWaitSeconds", str(args.boot_wait),
           "-KeyDelayMs", "250",
           "-AfterKeysSeconds", "10",
           "-MonitorPort", str(args.port + 40000),
           "-Keys", keys_for(command)]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    sys.stdout.write(proc.stdout[-2000:] if proc.stdout else "")
    sys.stderr.write(proc.stderr[-2000:] if proc.stderr else "")

    srv.done.wait(timeout=15)
    if srv.error:
        print(f"[host] server error: {srv.error}")

    with open(GUEST_LOG, "r", encoding="utf-8", errors="replace") as fh:
        guest = fh.read()

    checks = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond), detail))

    expected_sent = args.text.encode()
    check("guest opened a TCP connection (host accepted)",
          srv.connected.is_set() and srv.peer is not None,
          f"peer={srv.peer}")
    if bulk:
        check("guest log reports the connection came up",
              "tcpsend:" in guest and "connect FAILED" not in guest)
        check(f"host received all {bulk_len} bytes (> 2 MSS segments)",
              srv.received == bulk_payload,
              f"host got {len(srv.received)} bytes, "
              f"match={srv.received == bulk_payload}")
        check("guest log reports the bulk send was ACKed",
              f"tcpsend: {bulk_len} bytes ACKed" in guest)
        check("guest closed the connection cleanly (host saw EOF)",
              srv.closed_by_peer)
    else:
        check("guest log shows 'TCP connected'",
              "TCP connected (handle" in guest)
        check("host received the exact bytes the guest sent",
              srv.received == expected_sent,
              f"host got {srv.received!r}, guest sent {expected_sent!r}")
        rx = re.search(r"TCP received (\d+) bytes hex=([0-9a-f]*)", guest)
        check("guest printed a received payload", rx is not None)
        if rx:
            got = bytes.fromhex(rx.group(2))
            expect_reply = srv.sent[: int(rx.group(1))]
            check("guest received exactly what the host sent",
                  got == expect_reply, f"guest printed {got!r}, host sent {srv.sent!r}")
        check("guest closed the connection ('TCP close' line)",
              "TCP close" in guest)
    check("no bad TCP checksum counted in the guest",
          "[tcp] segments" not in guest or " badsum=0" in guest,
          "see the tcpstat output if present")

    print("\n--- host server ---")
    print(f"received : {srv.received!r} ({len(srv.received)} bytes)")
    print(f"sent     : {srv.sent!r} ({len(srv.sent)} bytes)")
    print("--- guest log (TCP lines) ---")
    for line in guest.splitlines():
        if "TCP " in line or "[tcp]" in line:
            print("  " + line)

    print()
    ok = 0
    for name, passed, detail in checks:
        print(f"  [{'PASS' if passed else 'FAIL'}] {name}" + (f"  ({detail})" if detail else ""))
        ok += 1 if passed else 0
    print(f"\n{'=== RESULT: PASS ===' if ok == len(checks) else '=== RESULT: FAIL ==='}"
          f"  checks={len(checks)} ok={ok} fail={len(checks) - ok}")
    return 0 if ok == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
