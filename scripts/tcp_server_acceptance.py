#!/usr/bin/env python3
"""Host-side acceptance test for the SutureOS TCP *server* path (passive open).

The guest runs `tcpserve <port>`, which calls tcp_listen()/tcp_listen_accept()
and echoes whatever arrives. QEMU's user-mode network is started with
`hostfwd=tcp::HOSTPORT-:GUESTPORT`, so this script - running on the host - can
open a real TCP connection that slirp forwards into the guest. That exercises:

  * the SYN -> SYN+ACK -> ACK handshake with the guest as the *passive* side,
  * tcp_listen_accept() handing the established child over,
  * data in both directions (host -> guest payload, guest -> host echo),
  * the guest closing the connection after the host's FIN.

Usage:
    py -3 scripts/tcp_server_acceptance.py [--host-port 5700] [--guest-port 40404]
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
GUEST_LOG = os.path.join(BUILD, "serial_tcpserve.log")
ARGS_FILE = os.path.join(BUILD, "qemu_tcpserve.args")

KEYMAP = {" ": "spc", ".": "dot", "/": "slash", "-": "minus", "=": "equal"}


def keys_for(text: str) -> str:
    out = []
    for ch in text:
        out.append(KEYMAP.get(ch, ch))
    out.append("ret")
    return ";".join(out)


class Client(threading.Thread):
    """Waits for the guest to announce its listener, then talks to it."""

    def __init__(self, host_port: int, guest_port: int, payload: bytes):
        super().__init__(daemon=True)
        self.host_port = host_port
        self.guest_port = guest_port
        self.payload = payload
        self.echo = b""
        self.connected = False
        self.error = None
        self.t_connected = None

    def run(self):
        banners = (f"TCP listening on port {self.guest_port}",
                   f"TCPAPI listening on port {self.guest_port}")
        deadline = time.time() + 90
        while time.time() < deadline:
            try:
                with open(GUEST_LOG, "r", encoding="utf-8", errors="replace") as fh:
                    text = fh.read()
                if any(b in text for b in banners):
                    break
            except OSError:
                pass
            time.sleep(0.25)
        else:
            self.error = "guest never announced its listener"
            return

        # The guest is listening now; give slirp a moment and retry briefly.
        last = None
        for _ in range(40):
            try:
                s = socket.create_connection(("127.0.0.1", self.host_port), timeout=5)
                break
            except OSError as exc:
                last = exc
                time.sleep(0.25)
        else:
            self.error = f"could not connect through hostfwd: {last}"
            return

        self.connected = True
        self.t_connected = time.time()
        try:
            s.settimeout(10)
            s.sendall(self.payload)
            got = b""
            while len(got) < len(self.payload):
                chunk = s.recv(4096)
                if not chunk:
                    break
                got += chunk
            self.echo = got
            s.shutdown(socket.SHUT_WR)          # host closes -> guest should too
            try:
                while s.recv(4096):
                    pass
            except socket.timeout:
                pass
            s.close()
        except Exception as exc:  # noqa: BLE001 - reported to the caller
            self.error = exc


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host-port", type=int, default=5700)
    ap.add_argument("--guest-port", type=int, default=40404)
    ap.add_argument("--payload", default="HELLO-FROM-HOST-42")
    ap.add_argument("--boot-wait", type=int, default=18)
    ap.add_argument("--command", default="tcpserve",
                    help="guest command prefix: tcpserve (raw tcp_* API) or "
                         "tcpapi (same server driven through the socket ABI)")
    args = ap.parse_args()

    payload = args.payload.encode()
    ARGS_FILE_CONTENT = (
        "-cdrom build/sutureos.iso -boot d -smp 2 -no-reboot -no-shutdown "
        f"-netdev user,id=n0,hostfwd=tcp::{args.host_port}-:{args.guest_port} "
        "-device rtl8139,netdev=n0"
    )
    os.makedirs(BUILD, exist_ok=True)
    with open(ARGS_FILE, "w", encoding="utf-8") as fh:
        fh.write(ARGS_FILE_CONTENT + "\n")
    if os.path.exists(GUEST_LOG):
        os.remove(GUEST_LOG)

    client = Client(args.host_port, args.guest_port, payload)
    client.start()
    time.sleep(0.5)

    command = f"{args.command.strip()} {args.guest_port}"
    print(f"[host] hostfwd 127.0.0.1:{args.host_port} -> guest :{args.guest_port}")
    print(f"[guest] will type: {command}")
    proc = subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
         "-File", os.path.join(ROOT, "scripts", "qemu_boot.ps1"),
         "-SerialLog", os.path.relpath(GUEST_LOG, ROOT),
         "-ArgFile", os.path.relpath(ARGS_FILE, ROOT),
         "-BootWaitSeconds", str(args.boot_wait),
         "-KeyDelayMs", "250",
         "-AfterKeysSeconds", "25",
         "-MonitorPort", str(args.host_port + 40000),
         "-Keys", keys_for(command)],
        cwd=ROOT, capture_output=True, text=True)
    sys.stdout.write(proc.stdout[-1500:] if proc.stdout else "")

    client.join(timeout=10)
    with open(GUEST_LOG, "r", encoding="utf-8", errors="replace") as fh:
        guest = fh.read()

    checks = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond), detail))

    check("guest announced its listener",
          f"TCP listening on port {args.guest_port}" in guest or
          f"TCPAPI listening on port {args.guest_port}" in guest)
    check("host TCP connection was established through hostfwd", client.connected,
          f"error={client.error}")
    check("guest accepted a connection (passive open completed)",
          "TCP accepted from" in guest or "TCPAPI accepted fd=" in guest)
    check("host received the echo byte-for-byte", client.echo == payload,
          f"host got {client.echo!r}, wanted {payload!r}")
    m = re.search(r"TCP(?:API| serve) rx (\d+) bytes hex=([0-9a-f]*)", guest)
    check("guest logged the payload it received", m is not None)
    if m:
        got = bytes.fromhex(m.group(2))
        check("guest received exactly what the host sent", got == payload,
              f"guest got {got!r}")
    check("guest finished the session cleanly",
          "TCP serve done: echoed" in guest or "TCPAPI done: echoed" in guest)
    check("no bad TCP checksum in the guest",
          "badsum" not in guest or " badsum=0" in guest)

    print("\n--- guest log (TCP lines) ---")
    for line in guest.splitlines():
        if "TCP" in line or "tcpserve" in line:
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
