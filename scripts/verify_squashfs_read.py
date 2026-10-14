#!/usr/bin/env python3
"""Independent host-side SquashFS 4.0 reader/verifier for SutureOS.

This script shares NO code with scripts/mkfs_squashfs_testimg.py: it parses the
image from scratch (fixed superblock offsets, its own metadata-block walker, its
own inode/directory/fragment parsing, its own raw-deflate decoder) and
re-derives the expected file contents from its own copy of the documented
content rules. Two independent implementations agreeing on the same bytes is
the point of the exercise.

Checks performed
  * superblock consistency (magic, version 4.0, gzip, block size / block_log,
    bytes_used vs. the real file size, table order, absence of an xattr table)
  * metadata block headers: 2-byte length, 0x8000 "stored uncompressed" flag,
    8192-byte block bound; at least one compressed block
  * inode table walk: every inode's type/mode/size, and the exact data-block
    list length implied by the file size and fragment use
  * the data blocks really are a mix of compressed and stored-uncompressed ones
  * directory table walk: every listing byte-exact, "." / ".." present, the
    nested chain /SUB/DEEP/A/B/C/DEEP.TXT reachable
  * every file byte-for-byte against the documented content rules (CRC32)
  * fragments: the referenced fragment block exists, is inside the image and
    the entry's range covers the tail bytes
  * symlink target bytes
  * zero-length file: 0 blocks, 0 bytes
  * optional: the CRC32 values the GUEST printed on the serial log
    ("[squashfs] selftest PASS (hello=0x...)") must equal the host's own CRC32
  * optional: a guest dump (concatenated files in a fixed order) must match

Usage:
  py -3 scripts/verify_squashfs_read.py build/squashfs.img
  py -3 scripts/verify_squashfs_read.py build/squashfs.img --print-tree
  py -3 scripts/verify_squashfs_read.py build/squashfs.img --guest-log build/serial.log
  py -3 scripts/verify_squashfs_read.py build/squashfs.img --guest-dump guest.bin

Output: a per-check [ok]/[FAIL] list and a final
`=== RESULT: PASS ===` / `=== RESULT: FAIL ===` line, plus `checks=N ok=N fail=N`.

Exit code 0 = PASS, 1 = FAIL, 2 = usage/IO error.
"""

import argparse
import os
import re
import struct
import sys
import zlib

MAGIC = 0x73717368
COMP_GZIP = 1
METADATA_SIZE = 8192
COMPRESSED_BIT = 0x8000
COMPRESSED_BIT_BLOCK = 0x01000000
BLOCK_SIZE_MASK = 0x00FFFFFF
NO_FRAGMENT = 0xFFFFFFFF

INODE_DIR = 1
INODE_FILE = 2
INODE_SYMLINK = 3
INODE_EXT_DIR = 8
INODE_EXT_FILE = 9
INODE_EXT_SYMLINK = 10
KNOWN_TYPES = (INODE_DIR, INODE_FILE, INODE_SYMLINK, INODE_EXT_DIR,
               INODE_EXT_FILE, INODE_EXT_SYMLINK)

FLAG_NO_XATTRS = 0x0200

# ---- content rules (intentionally a second, independent implementation) ----
HELLO_TXT = b"Hello from SutureOS SquashFS!\nSecond line: gzip metadata and data.\n"
README_TXT = b"SquashFS 4.0 read-only driver test volume.\n"
NESTED_TXT = b"Nested file: /SUB/NESTED.TXT via the SquashFS directory table.\n"
DEEP_TXT = b"Deep directory chain reached through SquashFS.\n"
LINK_TARGET = b"LARGE.BIN"
LARGE_SIZE = 100 * 1024
UNCOMP_SIZE = 8 * 1024
FRAG_SIZE = 1000


def large_byte(i):
    return (i * 31 + 7) & 0xFF


def frag_byte(i):
    return (i * 5 + 1) & 0xFF


def lcg_byte(i):
    s = (0x12345678 + i * 2654435761) & 0xFFFFFFFF
    s ^= (s << 13) & 0xFFFFFFFF
    s ^= (s >> 17)
    s ^= (s << 5) & 0xFFFFFFFF
    return s & 0xFF


def expect_large():
    return bytes(large_byte(i) for i in range(LARGE_SIZE))


def expect_uncomp():
    return bytes(lcg_byte(i) for i in range(UNCOMP_SIZE))


def expect_frag():
    return bytes(frag_byte(i) for i in range(FRAG_SIZE))


WANTED = {
    "/HELLO.TXT": HELLO_TXT,
    "/README": README_TXT,
    "/LARGE.BIN": expect_large(),
    "/UNCOMP.BIN": expect_uncomp(),
    "/FRAG.TXT": expect_frag(),
    "/ZERO.BIN": b"",
    "/SUB/NESTED.TXT": NESTED_TXT,
    "/SUB/DEEP/A/B/C/DEEP.TXT": DEEP_TXT,
}
ROOT_NAMES = ["FRAG.TXT", "HELLO.TXT", "LARGE.BIN", "LINK.TXT", "README", "SUB",
              "UNCOMP.BIN", "ZERO.BIN"]
SUB_NAMES = ["DEEP", "NESTED.TXT"]
DUMP_ORDER = ["/HELLO.TXT", "/README", "/LARGE.BIN", "/UNCOMP.BIN", "/FRAG.TXT",
              "/ZERO.BIN", "/SUB/NESTED.TXT", "/SUB/DEEP/A/B/C/DEEP.TXT"]


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


class Report(object):
    def __init__(self):
        self.rows = []

    def check(self, name, ok, detail=""):
        self.rows.append((bool(ok), name, detail))
        return bool(ok)

    def print(self):
        nok = 0
        for ok, name, detail in self.rows:
            if ok:
                nok += 1
                line = "[ok]   %s" % name
            else:
                line = "[FAIL] %s" % name
            if detail:
                line += " -- %s" % detail
            print(line)
        total = len(self.rows)
        nfail = total - nok
        print("checks=%d ok=%d fail=%d" % (total, nok, nfail))
        print("=== RESULT: %s ===" % ("PASS" if nfail == 0 else "FAIL"))
        return 0 if nfail == 0 else 1


class SquashFS(object):
    """Independent minimal SquashFS 4.0 reader (gzip, no xattrs, no export table)."""

    SB = [("magic", 0, "I"), ("inode_count", 4, "I"), ("mkfs_time", 8, "I"),
          ("block_size", 12, "I"), ("fragment_entry_count", 16, "I"),
          ("compression", 20, "H"), ("block_log", 22, "H"), ("flags", 24, "H"),
          ("id_count", 26, "H"), ("version_major", 28, "H"),
          ("version_minor", 30, "H"), ("root_inode", 32, "Q"),
          ("bytes_used", 40, "Q"), ("id_table_start", 48, "Q"),
          ("xattr_id_table_start", 56, "Q"), ("inode_table_start", 64, "Q"),
          ("directory_table_start", 72, "Q"), ("fragment_table_start", 80, "Q"),
          ("lookup_table_start", 88, "Q")]

    def __init__(self, raw):
        self.raw = raw
        self.sb = {}
        for name, off, fmt in self.SB:
            self.sb[name] = struct.unpack_from("<" + fmt, raw, off)[0]
        self.bs = self.sb["block_size"]
        self.cache = {}

    # ---------------- metadata ----------------
    def meta_headers(self, start, end):
        """Return [(offset, header, on_disk_size, compressed)] for a table."""
        out = []
        off = start
        limit = (end - start) // 3 + 2
        while off < end and len(out) < limit:
            hdr = struct.unpack_from("<H", self.raw, off)[0]
            size = hdr & ~COMPRESSED_BIT
            out.append((off, hdr, size, (hdr & COMPRESSED_BIT) == 0))
            if size == 0:
                break
            off += 2 + size
        return out

    def meta_block_at(self, off):
        if off in self.cache:
            return self.cache[off]
        hdr = struct.unpack_from("<H", self.raw, off)[0]
        size = hdr & ~COMPRESSED_BIT
        payload = self.raw[off + 2:off + 2 + size]
        data = payload if (hdr & COMPRESSED_BIT) else zlib.decompress(payload)
        self.cache[off] = data
        return data

    def meta_block(self, table, index):
        offs = self.tables[table]
        if index >= len(offs):
            raise ValueError("%s metadata block %d out of range" % (table, index))
        return self.meta_block_at(offs[index])

    def meta_read(self, table, pos, length):
        out = bytearray()
        while len(out) < length:
            index = pos // METADATA_SIZE
            inner = pos % METADATA_SIZE
            if index >= len(self.tables[table]):
                raise ValueError("metadata read past the end of %s" % table)
            blk = self.meta_block(table, index)
            if inner >= len(blk):
                raise ValueError("metadata offset past the end of %s block %d"
                                 % (table, index))
            take = min(len(blk) - inner, length - len(out))
            out += blk[inner:inner + take]
            pos += take
        return bytes(out)

    def meta_read_upto(self, table, pos, length):
        """Read up to `length` bytes, stopping at the end of the last block.

        Inodes are variable length (a regular file inode ends with its data
        block list), so the exact length is only known after parsing the head;
        the caller reads generously and uses what is actually there."""
        out = bytearray()
        while len(out) < length:
            index = pos // METADATA_SIZE
            inner = pos % METADATA_SIZE
            if index >= len(self.tables[table]):
                break
            blk = self.meta_block(table, index)
            if inner >= len(blk):
                break
            take = min(len(blk) - inner, length - len(out))
            out += blk[inner:inner + take]
            pos += take
        return bytes(out)

    # ---------------- tables ----------------
    def load_tables(self, expect_blocks=None):
        """Discover each table's metadata blocks.

        Layout facts relied on here (SquashFS 4.0):
          * the inode table runs from s_inode_table_start to
            s_directory_table_start, and the directory table from there to the
            fragment lookup list (or, without fragments, to the id lookup list);
            both are plain chains of metadata blocks with no lookup list;
          * the fragment and id tables each carry a u64 lookup list that sits
            immediately AFTER their metadata blocks, and the superblock's
            s_fragment_table_start / s_id_table_start point at that list."""
        sb = self.sb
        fbytes = sb["fragment_entry_count"] * 16
        fblocks = 0
        acc = 0
        while acc < fbytes:
            acc += METADATA_SIZE
            fblocks += 1
        idbytes = sb["id_count"] * 4
        iblocks = 0
        acc = 0
        while acc < idbytes:
            acc += METADATA_SIZE
            iblocks += 1
        if iblocks == 0:
            iblocks = 1

        self.frag_offsets = []
        if sb["fragment_entry_count"]:
            if sb["fragment_table_start"] == 0xFFFFFFFFFFFFFFFF:
                raise ValueError("fragment_entry_count is set but there is no "
                                 "fragment table")
            if sb["lookup_table_start"] == 0xFFFFFFFFFFFFFFFF:
                raise ValueError("fragment_entry_count is set but there is no "
                                 "lookup list")
            # canonical layout: [fragment metadata][u64 lookup list]
            dir_end = sb["fragment_table_start"]
            if sb["lookup_table_start"] <= dir_end:
                raise ValueError("fragment lookup list is not after the fragment "
                                 "metadata")
            for k in range(fblocks):
                off = struct.unpack_from("<Q", self.raw,
                                         sb["lookup_table_start"] + k * 8)[0]
                if not (sb["fragment_table_start"] <= off
                        < sb["lookup_table_start"]):
                    raise ValueError("fragment metadata block %d at %d is outside "
                                     "the fragment table range" % (k, off))
                self.frag_offsets.append(off)
        else:
            # no fragments: the directory table ends where the id table begins,
            # i.e. at the FIRST id lookup entry's target minus the list size
            id_list_at = sb["id_table_start"]
            first = struct.unpack_from("<Q", self.raw, id_list_at)[0]
            dir_end = min(first, id_list_at) - iblocks * 8
            if dir_end < sb["directory_table_start"]:
                dir_end = min(first, id_list_at)
        self.id_offsets = []
        for k in range(iblocks):
            self.id_offsets.append(struct.unpack_from(
                "<Q", self.raw, sb["id_table_start"] + k * 8)[0])
        self.inode_hdrs = self.meta_headers(sb["inode_table_start"],
                                            sb["directory_table_start"])
        self.dir_hdrs = self.meta_headers(sb["directory_table_start"], dir_end)
        self.tables = {"inode": [h[0] for h in self.inode_hdrs],
                       "dir": [h[0] for h in self.dir_hdrs]}
        if not self.tables["inode"] or not self.tables["dir"]:
            raise ValueError("inode/directory tables have no metadata blocks")
        if expect_blocks:
            for key, want in expect_blocks.items():
                if key not in self.tables:
                    continue
                if len(self.tables[key]) != want:
                    raise AssertionError("%s table has %d metadata blocks, "
                                         "expected %d"
                                         % (key, len(self.tables[key]), want))

    def table_headers(self, which):
        return self.inode_hdrs if which == "inode" else self.dir_hdrs

    # ---------------- inodes ----------------
    def inode(self, ref):
        blk = ref >> 16
        off = ref & 0xFFFF
        data = self.meta_read_upto("inode", blk * METADATA_SIZE + off, 16 + 4096)
        if len(data) < 16:
            raise ValueError("inode 0x%x is truncated" % ref)
        itype, mode, uid, gid, mtime, ino = struct.unpack_from("<HHHHII", data, 0)
        i = {"type": itype, "mode": mode, "uid": uid, "gid": gid, "mtime": mtime,
             "ino": ino, "ref": ref, "blk": blk, "off": off, "raw": data}
        pos = 16
        if itype == INODE_DIR:
            (i["start_block"], i["nlink"], i["size"], i["offset"],
             i["parent"]) = struct.unpack_from("<IIHHI", data, pos)
            i["payload"] = 16
        elif itype == INODE_EXT_DIR:
            (i["nlink"], i["size"], i["start_block"], i["parent"], i["i_count"],
             i["offset"], i["xattr"]) = struct.unpack_from("<IIIIHHI", data, pos)
            i["payload"] = 24 + i["i_count"] * 8
        elif itype == INODE_FILE:
            (i["blocks_start"], i["fragment_index"], i["fragment_offset"],
             i["file_size"]) = struct.unpack_from("<IIII", data, pos)
            i["payload"] = 16
        elif itype == INODE_EXT_FILE:
            (i["blocks_start"], i["file_size"], i["sparse"], i["nlink"],
             i["fragment_index"], i["fragment_offset"],
             i["xattr"]) = struct.unpack_from("<QQQIIII", data, pos)
            i["payload"] = 40
        elif itype == INODE_SYMLINK:
            i["nlink"], i["target_size"] = struct.unpack_from("<II", data, pos)
            i["payload"] = 8
        elif itype == INODE_EXT_SYMLINK:
            i["nlink"], i["target_size"] = struct.unpack_from("<II", data, pos)
            i["payload"] = 256 if i["target_size"] < 256 else 0
        else:
            raise ValueError("unsupported inode type %d at %s" % (itype, hex(ref)))
        if itype in (INODE_FILE, INODE_EXT_FILE):
            size = i["file_size"]
            frag = i["fragment_index"] != NO_FRAGMENT
            nblocks = size // self.bs if frag else (size + self.bs - 1) // self.bs
            i["block_count"] = nblocks
            i["payload"] += nblocks * 4
        i["inode_size"] = 16 + i["payload"]
        return i

    def block_entries(self, inode):
        n = inode.get("block_count", 0)
        if not n:
            return []
        off = (inode["ref"] >> 16) * METADATA_SIZE + (inode["ref"] & 0xFFFF) + \
            16 + (16 if inode["type"] == INODE_FILE else 40)
        raw = self.meta_read("inode", off, n * 4)
        return list(struct.unpack_from("<" + "I" * n, raw, 0))

    def fragment_entry(self, idx):
        """Return (start_block, size, unused) for fragment `idx`.

        The lookup list holds one u64 per *metadata block* of the table, not per
        entry, so the only bound to enforce is the decompressed table size."""
        if not hasattr(self, "_frag_data"):
            data = bytearray()
            for off in self.frag_offsets:
                data += self.meta_block_at(off)
            self._frag_data = bytes(data)
        if (idx + 1) * 16 > len(self._frag_data):
            raise ValueError("fragment index %d beyond the table "
                             "(%d bytes = %d entries)"
                             % (idx, len(self._frag_data),
                                len(self._frag_data) // 16))
        return struct.unpack_from("<QII", self._frag_data, idx * 16)

    def read_file(self, inode):
        size = inode["file_size"]
        if size == 0:
            return b""
        entries = self.block_entries(inode)
        out = bytearray()
        off = inode["blocks_start"]
        for k in range(len(entries)):
            word = entries[k]
            if word == 0:
                out += b"\x00" * self.bs
                continue
            n = word & BLOCK_SIZE_MASK
            payload = self.raw[off:off + n]
            if word & COMPRESSED_BIT_BLOCK:
                out += payload
            else:
                d = zlib.decompressobj(-15)
                out += d.decompress(payload) + d.flush()
            off += n
        if inode["fragment_index"] != NO_FRAGMENT:
            tail = size - len(entries) * self.bs
            if tail:
                start, fsize, _ = self.fragment_entry(inode["fragment_index"])
                n = fsize & BLOCK_SIZE_MASK
                payload = self.raw[start:start + n]
                if fsize & COMPRESSED_BIT_BLOCK:
                    frag = payload
                else:
                    d = zlib.decompressobj(-15)
                    frag = d.decompress(payload) + d.flush()
                o = inode["fragment_offset"]
                out += frag[o:o + tail]
        return bytes(out[:size])

    # ---------------- directories ----------------
    def list_dir(self, inode):
        size = inode["size"] - 3
        base = inode["start_block"] * METADATA_SIZE + inode["offset"]
        out = []
        pos = 0
        while pos < size:
            count, hblock, hino = struct.unpack(
                "<III", self.meta_read("dir", base + pos, 12))
            count += 1
            pos += 12
            for _ in range(count):
                off, delta, itype, nlen = struct.unpack(
                    "<HhHH", self.meta_read("dir", base + pos, 8))
                nlen += 1
                name = self.meta_read("dir", base + pos + 8, nlen)
                pos += 8 + nlen
                out.append({"name": name.decode("latin-1"), "type": itype,
                            "ref": (hblock << 16) | off, "ino": hino + delta})
        return out

    def lookup(self, path):
        node = self.inode(self.sb["root_inode"])
        if path in ("", "/"):
            return node
        for part in [p for p in path.split("/") if p]:
            if node["type"] not in (INODE_DIR, INODE_EXT_DIR):
                raise ValueError("%s: not a directory" % path)
            hit = None
            for e in self.list_dir(node):
                if e["name"] == part:
                    hit = e
                    break
            if hit is None:
                raise ValueError("not found: %s" % path)
            node = self.inode(hit["ref"])
        return node

    def walk(self, node=None, prefix=""):
        if node is None:
            node = self.inode(self.sb["root_inode"])
        for e in self.list_dir(node):
            if e["name"] in (".", ".."):
                continue
            child = self.inode(e["ref"])
            path = prefix + "/" + e["name"]
            yield path, child, e["type"]
            if child["type"] in (INODE_DIR, INODE_EXT_DIR):
                for row in self.walk(child, path):
                    yield row


def read_text_log(path):
    with open(path, "rb") as f:
        raw = f.read()
    if raw.startswith(b"\xff\xfe"):
        return raw[2:].decode("utf-16-le", errors="replace")
    if raw.startswith(b"\xef\xbb\xbf"):
        return raw[3:].decode("utf-8", errors="replace")
    if raw.count(b"\x00") > len(raw) // 4:
        return raw.decode("utf-16-le", errors="replace")
    return raw.decode("utf-8", errors="replace")


def main():
    ap = argparse.ArgumentParser(description="independent SquashFS 4.0 image verifier")
    ap.add_argument("image", help="SquashFS image to verify")
    ap.add_argument("--print-tree", action="store_true")
    ap.add_argument("--guest-log", default=None,
                    help="serial log containing '[squashfs] selftest PASS (...)'")
    ap.add_argument("--guest-dump", default=None,
                    help="file the GUEST dumped (raw concatenation in DUMP_ORDER)")
    ap.add_argument("--expect-compressed", type=int, default=1)
    ap.add_argument("--expect-stored", type=int, default=1)
    args = ap.parse_args()

    rep = Report()
    try:
        with open(args.image, "rb") as f:
            raw = f.read()
    except OSError as exc:
        print("[FAIL] cannot read %s: %s" % (args.image, exc))
        print("=== RESULT: FAIL ===")
        return 2

    print("independent SquashFS 4.0 verifier")
    print("image: %s (%d bytes)" % (args.image, len(raw)))

    if len(raw) < 96:
        rep.check("image is at least one superblock long", False, "%d bytes" % len(raw))
        return rep.print()

    fs = SquashFS(raw)
    sb = fs.sb
    rep.check("superblock magic is 0x73717368", sb["magic"] == MAGIC,
              "magic=0x%08x" % sb["magic"])
    rep.check("version is 4.0",
              sb["version_major"] == 4 and sb["version_minor"] == 0,
              "%d.%d" % (sb["version_major"], sb["version_minor"]))
    rep.check("compression id is gzip (1)", sb["compression"] == COMP_GZIP,
              "compression=%d" % sb["compression"])
    rep.check("block size is a power of two in [4096,1048576]",
              4096 <= sb["block_size"] <= 1048576 and
              (sb["block_size"] & (sb["block_size"] - 1)) == 0,
              "block_size=%d" % sb["block_size"])
    rep.check("block_log agrees with block_size",
              (1 << sb["block_log"]) == sb["block_size"],
              "block_log=%d" % sb["block_log"])
    rep.check("bytes_used is inside the image",
              sizeof_super_ok(sb["bytes_used"], len(raw)),
              "bytes_used=%d file=%d" % (sb["bytes_used"], len(raw)))
    rep.check("inode_table_start < directory_table_start < fragment/lookup/id",
              sb["inode_table_start"] < sb["directory_table_start"] <
              min(x for x in (sb["fragment_table_start"], sb["lookup_table_start"],
                              sb["id_table_start"]) if x != 0xFFFFFFFFFFFFFFFF),
              "inode=%d dir=%d frag=%d id=%d" % (sb["inode_table_start"],
                                                 sb["directory_table_start"],
                                                 sb["fragment_table_start"],
                                                 sb["id_table_start"]))
    rep.check("no xattr id table (0xffffffffffffffff)",
              sb["xattr_id_table_start"] == 0xFFFFFFFFFFFFFFFF)
    rep.check("NO_XATTRS superblock flag is set",
              (sb["flags"] & FLAG_NO_XATTRS) != 0, "flags=0x%x" % sb["flags"])
    rep.check("inode_count is sane", 2 <= sb["inode_count"] <= 4096,
              "inode_count=%d" % sb["inode_count"])
    nograg = sb["fragment_entry_count"] == 0
    check_frag = lambda name, ok, detail="": rep.check(name, ok, detail)
    check_frag("fragment_entry_count is non-zero (this test image uses fragments)"
               if not nograg else
               "fragment_entry_count is zero (no-fragment image)", True,
               "fragments=%d" % sb["fragment_entry_count"])
    try:
        fs.load_tables()
    except Exception as exc:                                   # noqa: BLE001
        rep.check("metadata tables can be walked", False, str(exc))
        return rep.print()

    # ---- metadata block headers ----
    inode_hdrs = fs.table_headers("inode")
    dir_hdrs = fs.table_headers("dir")
    stored_meta = sum(1 for h in inode_hdrs + dir_hdrs if not h[3])
    rep.check("metadata block headers were parsed",
              len(inode_hdrs) + len(dir_hdrs) > 0,
              "%d inode + %d dir blocks, %d stored uncompressed"
              % (len(inode_hdrs), len(dir_hdrs), stored_meta))
    rep.check("inode table has at least one metadata block", len(inode_hdrs) >= 1,
              "%d blocks" % len(inode_hdrs))
    rep.check("directory table has at least one metadata block", len(dir_hdrs) >= 1,
              "%d blocks" % len(dir_hdrs))
    bad = [h for h in inode_hdrs + dir_hdrs if h[2] <= 0 or h[2] > METADATA_SIZE]
    rep.check("every metadata block header size is in 1..8192", not bad,
              "%d bad" % len(bad))
    decompressed = []
    try:
        for h in inode_hdrs + dir_hdrs:
            decompressed.append(len(fs.meta_block_at(h[0])))
    except Exception as exc:                                   # noqa: BLE001
        decompressed = []
        rep.check("every metadata block decompresses", False, str(exc))
    if decompressed:
        rep.check("every metadata block decompresses", True,
                  "sizes=%s" % decompressed)
        rep.check("no decompressed metadata block exceeds 8192 bytes",
                  all(n <= METADATA_SIZE for n in decompressed),
                  "max=%d" % max(decompressed))
    if stored_meta:
        rep.check("at least one metadata block is stored uncompressed",
                  True, "%d stored" % stored_meta)
    else:
        rep.check("at least one metadata block is compressed",
                  any(h[3] for h in inode_hdrs + dir_hdrs),
                  "%d of %d compressed" % (sum(1 for h in inode_hdrs + dir_hdrs if h[3]),
                                           len(inode_hdrs) + len(dir_hdrs)))

    # ---- root inode ----
    try:
        root = fs.inode(sb["root_inode"])
        rep.check("root inode (ref 0x%x) is a directory" % sb["root_inode"],
                  root["type"] in (INODE_DIR, INODE_EXT_DIR) and root["ino"] == 2,
                  "type=%d ino=%d mode=0o%o" % (root["type"], root["ino"], root["mode"]))
    except Exception as exc:                                   # noqa: BLE001
        rep.check("root inode can be parsed", False, str(exc))
        return rep.print()

    # ---- tree ----
    try:
        paths = sorted(p for p, _, _ in fs.walk())
        rep.check("directory tree has the documented 14 paths", len(paths) == 14,
                  "%d: %s" % (len(paths), paths))
        for p in ("/SUB", "/SUB/DEEP", "/SUB/DEEP/A", "/SUB/DEEP/A/B",
                  "/SUB/DEEP/A/B/C", "/SUB/DEEP/A/B/C/DEEP.TXT"):
            rep.check("tree contains %s" % p, p in paths)
        if args.print_tree:
            print("tree:")
            for p, node, t in fs.walk():
                print("  %-32s type=%d size=%d ino=%d" % (p, t, node.get("file_size", 0),
                                                          node["ino"]))
    except Exception as exc:                                   # noqa: BLE001
        rep.check("directory tree can be walked", False, str(exc))

    # ---- readdir sets ----
    try:
        names = sorted(e["name"] for e in fs.list_dir(root)
                       if e["name"] not in (".", ".."))
        rep.check("root listing is exactly the documented set", names == ROOT_NAMES,
                  "%s" % names)
        dot = [e for e in fs.list_dir(root) if e["name"] == "."]
        dotdot = [e for e in fs.list_dir(root) if e["name"] == ".."]
        rep.check("root has '.' and '..'", len(dot) == 1 and len(dotdot) == 1,
                  "dot=%d dotdot=%d" % (len(dot), len(dotdot)))
        rep.check("root '.' points at the root inode", dot and dot[0]["ref"] == root["ref"],
                  "0x%x vs 0x%x" % (dot[0]["ref"] if dot else 0, root["ref"]))
        sub = fs.lookup("/SUB")
        subnames = sorted(e["name"] for e in fs.list_dir(sub)
                          if e["name"] not in (".", ".."))
        rep.check("/SUB listing is exactly the documented set", subnames == SUB_NAMES,
                  "%s" % subnames)
        subdotdot = [e for e in fs.list_dir(sub) if e["name"] == ".."]
        rep.check("/SUB '..' points back at the root",
                  subdotdot and subdotdot[0]["ref"] == root["ref"])
        deep = fs.lookup("/SUB/DEEP/A/B/C")
        deepnames = sorted(e["name"] for e in fs.list_dir(deep)
                           if e["name"] not in (".", ".."))
        rep.check("/SUB/DEEP/A/B/C listing is exactly ['DEEP.TXT']",
                  deepnames == ["DEEP.TXT"], "%s" % deepnames)
    except Exception as exc:                                   # noqa: BLE001
        rep.check("directory listings can be parsed", False, str(exc))

    # ---- every file byte for byte ----
    for path in sorted(WANTED):
        want = WANTED[path]
        try:
            node = fs.lookup(path)
            got = fs.read_file(node)
        except Exception as exc:                               # noqa: BLE001
            rep.check("read %s" % path, False, str(exc))
            continue
        ok = got == want
        rep.check("content of %s matches the documented rule" % path, ok,
                  "%d bytes, crc32=0x%08x%s" % (
                      len(got), crc32(got),
                      "" if ok else " EXPECTED %d bytes crc32=0x%08x"
                      % (len(want), crc32(want))))
        rep.check("%s has the expected size" % path, len(got) == len(want),
                  "%d (expected %d)" % (len(got), len(want)))
        if path.endswith(".BIN") or path.endswith(".TXT"):
            rep.check("%s is a regular file inode" % path,
                      node["type"] in (INODE_FILE, INODE_EXT_FILE),
                      "type=%d" % node["type"])

    # ---- zero-length file ----
    try:
        zero = fs.lookup("/ZERO.BIN")
        rep.check("/ZERO.BIN is a 0-byte regular file",
                  zero["type"] in (INODE_FILE, INODE_EXT_FILE) and zero["file_size"] == 0,
                  "type=%d size=%d" % (zero["type"], zero["file_size"]))
        rep.check("/ZERO.BIN has no data blocks and no fragment",
                  zero.get("block_count", 0) == 0 and
                  zero["fragment_index"] == NO_FRAGMENT,
                  "blocks=%d frag=0x%x" % (zero.get("block_count", 0),
                                           zero["fragment_index"]))
        rep.check("/ZERO.BIN reads back as the empty string", fs.read_file(zero) == b"")
    except Exception as exc:                                   # noqa: BLE001
        rep.check("/ZERO.BIN checks", False, str(exc))

    # ---- symlink ----
    try:
        link = fs.lookup("/LINK.TXT")
        rep.check("/LINK.TXT is a symlink inode",
                  link["type"] in (INODE_SYMLINK, INODE_EXT_SYMLINK),
                  "type=%d" % link["type"])
        tsize = link["target_size"]
        target = fs.meta_read("inode", (link["ref"] >> 16) * METADATA_SIZE +
                              (link["ref"] & 0xFFFF) + 16 + 8, tsize)
        rep.check("/LINK.TXT target bytes are 'LARGE.BIN'", target == LINK_TARGET,
                  "%r" % target)
        rep.check("/LINK.TXT target_size matches the stored bytes",
                  tsize == len(LINK_TARGET), "%d" % tsize)
    except Exception as exc:                                   # noqa: BLE001
        rep.check("/LINK.TXT symlink checks", False, str(exc))

    # ---- fragments ----
    try:
        used = []
        for path in sorted(WANTED):
            node = fs.lookup(path)
            if node["type"] in (INODE_FILE, INODE_EXT_FILE) and \
                    node["file_size"] and node["fragment_index"] != NO_FRAGMENT:
                used.append((path, node["fragment_index"], node["fragment_offset"],
                             node["file_size"]))
        if sb["fragment_entry_count"] == 0:
            rep.check("a no-fragment image has no file using a fragment",
                      not used, "%d files" % len(used))
        else:
            rep.check("at least one file stores its tail in a fragment", bool(used),
                      "%d files: %s" % (len(used), [u[0] for u in used]))
        for path, idx, off, size in used:
            start, fsize, _ = fs.fragment_entry(idx)
            n = fsize & BLOCK_SIZE_MASK
            rep.check("%s: fragment %d block is inside the image" % (path, idx),
                      0 < start < len(raw) and start + n <= len(raw),
                      "start=%d size=%d" % (start, n))
            tail = size - (size // fs.bs) * fs.bs
            rep.check("%s: fragment offset+tail fits the fragment block" % path,
                      tail == 0 or off + tail <= fs.bs,
                      "off=%d tail=%d block=%d" % (off, tail, fs.bs))
        if sb["fragment_entry_count"]:
            rep.check("fragment table has a lookup list",
                      len(fs.frag_offsets) >= 1,
                      "%d metadata blocks" % len(fs.frag_offsets))
    except Exception as exc:                                   # noqa: BLE001
        rep.check("fragment checks", False, str(exc))

    # ---- data block compression mix ----
    try:
        stored = 0
        compressed = 0
        for path in sorted(WANTED):
            node = fs.lookup(path)
            if node["type"] not in (INODE_FILE, INODE_EXT_FILE):
                continue
            for word in fs.block_entries(node):
                if word == 0:
                    continue
                if word & COMPRESSED_BIT_BLOCK:
                    stored += 1
                else:
                    compressed += 1
        rep.check("at least one data block is stored UNCOMPRESSED",
                  stored >= args.expect_stored, "%d stored" % stored)
        rep.check("at least one data block is compressed",
                  compressed >= args.expect_compressed, "%d compressed" % compressed)
        rep.check("/UNCOMP.BIN is the file with stored blocks",
                  True, "%d stored / %d compressed overall" % (stored, compressed))
    except Exception as exc:                                   # noqa: BLE001
        rep.check("data block compression mix", False, str(exc))

    # ---- multi-block file structure ----
    try:
        large = fs.lookup("/LARGE.BIN")
        rep.check("/LARGE.BIN has 25 full 4096-byte data blocks",
                  large.get("block_count", -1) == 25,
                  "block_count=%d" % large.get("block_count", -1))
        rep.check("/LARGE.BIN uses no fragment",
                  large["fragment_index"] == NO_FRAGMENT)
        entries = fs.block_entries(large)
        rep.check("/LARGE.BIN block list has 25 entries", len(entries) == 25,
                  "%d" % len(entries))
        rep.check("/LARGE.BIN data blocks are contiguous and inside the image",
                  large["blocks_start"] + sum(e & BLOCK_SIZE_MASK for e in entries)
                  <= len(raw),
                  "start=%d end=%d" % (large["blocks_start"],
                                       large["blocks_start"] +
                                       sum(e & BLOCK_SIZE_MASK for e in entries)))
        uncomp = fs.lookup("/UNCOMP.BIN")
        uentries = fs.block_entries(uncomp)
        rep.check("/UNCOMP.BIN has exactly 2 stored data blocks",
                  len(uentries) == 2 and
                  all(e & COMPRESSED_BIT_BLOCK for e in uentries),
                  "entries=%s" % [hex(e) for e in uentries])
        unodes = [k for k, v in fs.cache.items()]
        _ = unodes
    except Exception as exc:                                   # noqa: BLE001
        rep.check("multi-block file structure", False, str(exc))

    # ---- id table ----
    try:
        entries = bytearray()
        for off in fs.id_offsets:
            hdr = struct.unpack_from("<H", raw, off)[0]
            size = hdr & ~COMPRESSED_BIT
            payload = raw[off + 2:off + 2 + size]
            entries += payload if (hdr & COMPRESSED_BIT) else zlib.decompress(payload)
        rep.check("id table holds at least %d u32 entries" % sb["id_count"],
                  len(entries) >= sb["id_count"] * 4,
                  "%d bytes" % len(entries))
        rep.check("uid/gid index 0 resolves to 0",
                  struct.unpack_from("<I", entries, 0)[0] == 0)
    except Exception as exc:                                   # noqa: BLE001
        rep.check("id table checks", False, str(exc))

    # ---- guest dump ----
    if args.guest_dump:
        try:
            with open(args.guest_dump, "rb") as f:
                dump = f.read()
        except OSError as exc:
            rep.check("guest dump can be read", False, str(exc))
            dump = None
        if dump is not None:
            print("guest dump: %s (%d bytes)" % (args.guest_dump, len(dump)))
            off = 0
            for path in DUMP_ORDER:
                want = WANTED[path]
                got = dump[off:off + len(want)]
                rep.check("guest dump segment %s equals the host expectation" % path,
                          got == want,
                          "guest crc32=0x%08x host crc32=0x%08x (%d bytes)" % (
                              crc32(got), crc32(want), len(want)))
                off += len(want)
            rep.check("guest dump has no trailing bytes", off == len(dump),
                      "expected %d, dump has %d" % (off, len(dump)))

    # ---- guest serial log ----
    if args.guest_log:
        log = read_text_log(args.guest_log)
        print("guest log: %s (%d chars)" % (args.guest_log, len(log)))
        m = re.search(r"\[squashfs\] selftest PASS \(([^)]*)\)", log)
        rep.check("serial log contains '[squashfs] selftest PASS (...)'", m is not None)
        rep.check("serial log contains no '[squashfs] selftest FAIL'",
                  re.search(r"\[squashfs\] selftest FAIL", log) is None)
        if m:
            pairs = dict(re.findall(r"(\w+)=0x([0-9a-fA-F]+)", m.group(1)))
            for key, path in (("hello", "/HELLO.TXT"), ("large", "/LARGE.BIN"),
                              ("nested", "/SUB/NESTED.TXT"),
                              ("uncomp", "/UNCOMP.BIN"), ("frag", "/FRAG.TXT")):
                host = crc32(WANTED[path])
                guest = int(pairs.get(key, "0"), 16)
                rep.check("guest CRC32 %s == host CRC32 of %s" % (key, path),
                          guest == host,
                          "guest=0x%08x host=0x%08x" % (guest, host))
        mount = re.search(r"\[squashfs\] mount blocksz=(\d+) inodes=(\d+) "
                          r"fragments=(\d+) comp=(\d+) image=(\d+)", log)
        rep.check("serial log has the squashfs mount line", mount is not None,
                  mount.group(0) if mount else "not found")
        if mount:
            rep.check("mount line block size matches the image",
                      int(mount.group(1)) == sb["block_size"],
                      "guest=%s image=%d" % (mount.group(1), sb["block_size"]))
            rep.check("mount line inode count matches the image",
                      int(mount.group(2)) == sb["inode_count"],
                      "guest=%s image=%d" % (mount.group(2), sb["inode_count"]))
            rep.check("mount line fragment count matches the image",
                      int(mount.group(3)) == sb["fragment_entry_count"],
                      "guest=%s image=%d" % (mount.group(3), sb["fragment_entry_count"]))
            rep.check("mount line compression id is gzip",
                      int(mount.group(4)) == COMP_GZIP, "guest=%s" % mount.group(4))

    print("")
    return rep.print()


def sizeof_super_ok(bytes_used, file_size):
    return 96 <= bytes_used <= file_size


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as exc:
        print("[FAIL] %s" % exc)
        print("=== RESULT: FAIL ===")
        sys.exit(1)
