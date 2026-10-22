#!/usr/bin/env python3
"""Build the JSON payload for a GitHub release.

Kept as a separate script on purpose: assembling the JSON inside PowerShell
(`ConvertTo-Json` + `Invoke-RestMethod -Body`) blew the body up to ~500 KB and
GitHub rejected it with "body is too long (maximum is 125000 characters)".
Writing the payload here keeps the UTF-8 text byte-exact.

Usage:
    py -3 scripts/make_release_json.py --tag v0.6.0-alpha --name "SutureOS ..." \
        --notes docs/release-notes-v0.6.0-alpha.md --out build/release.json
"""
import argparse
import io
import json


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--notes", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--prerelease", action="store_true")
    ap.add_argument("--draft", action="store_true")
    args = ap.parse_args()

    with io.open(args.notes, encoding="utf-8") as fh:
        body = fh.read()

    payload = {
        "tag_name": args.tag,
        "name": args.name,
        "body": body,
        "draft": bool(args.draft),
        "prerelease": bool(args.prerelease),
    }
    with io.open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(json.dumps(payload, ensure_ascii=False))
    print("wrote %s (%d bytes body, %d bytes json)"
          % (args.out, len(body), len(json.dumps(payload, ensure_ascii=False))))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
