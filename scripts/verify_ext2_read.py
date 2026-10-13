#!/usr/bin/env python3
"""Independent host-side ext2 reader/verifier for SutureOS.

This script deliberately shares NO code with scripts/mkfs_ext2_testimg.py: it
parses the image from scratch (fixed on-disk offsets, its own block-pointer
walker) and re-derives the expected file contents from its own copy of the
documented content rules.  Two independent implementations agreeing on the
same bytes is the point of the exercise.

Checks performed
  * superblock consistency (magic, revision, block size, counts, features,
    filesystem state, blocks_count vs. real image size)
  * block bitmap / inode bitmap vs. the superblock + group descriptor counters
  * directory structure: root ".", ".."  and SUB/, "." ".." + NESTED.TXT
  * every file byte-for-byte against the documented content rules
  * indirect traversal correctness: BIG.BIN must resolve logical blocks
    through i_block[12] (single indirect) and DOUBLE.BIN through i_block[13]
    (double indirect tree) when the block size makes that necessary
  * sparse hole handling (i_block[1] == 0 reads back as zeros)
  * allocation sanity: no data block is out of range or shared by two files
  * optional: the bytes the GUEST dumped must equal the host expectation
  * optional: the CRC32 values the guest printed on the serial log
    ("[ext2] selftest PASS (hello=0x...)") must equal the host's own CRC32

Usage:
  py -3 scripts/verify_ext2_read.py build/ext2.img
  py -3 scripts/verify_ext2_read.py build/ext2.img --guest-dump guest.bin
  py -3 scripts/verify_ext2_read.py build/ext2.img --guest-log build/serial.log
  py -3 scripts/verify_ext2_read.py <foreign.img> --print-tree

Output: a per-check [ok]/[FAIL] list and a final
`=== RESULT: PASS ===` / `=== RESULT: FAIL ===` line.

Exit code 0 = PASS, 1 = FAIL, 2 = usage/IO error.
"""

import argparse
import os
import re
import struct
import sys
import zlib

SECTOR = 512
EXT2_MAGIC = 0xEF53
EXT2_VALID_FS = 0x0001
EXT2_ERROR_FS = 0x0002
EXT2_FEATURE_INCOMPAT_FILETYPE = 0x0002
# Features this read-only driver does NOT understand; a foreign image carrying
# them is out of scope rather than "verified".
UNSUPPORTED_INCOMPAT = 0x0004 | 0x0008 | 0x0010 | 0x0040 | 0x0080 | 0x0100

FT_UNKNOWN, FT_REG, FT_DIR = 0, 1, 2

# ---- content rules (intentionally a second, independent implementation) ----
HELLO_TXT = b"Hello from SutureOS ext2!\nSecond line: read via the real ext2 driver.\n"
NESTED_TXT = b"Nested file: SUB/NESTED.TXT reached through ext2 directory traversal.\n"
BIG_BIN_SIZE = 64 * 1024
DOUBLE_BIN_SIZE = 300 * 1024
SPARSE_BIN_BLOCKS = 3
SPARSE_HOLE_BLOCK = 1


def expect_big():
    return bytes((i * 31 + 7) & 0xFF for i in range(BIG_BIN_SIZE))


def expect_double():
    return bytes((i * 29 + 11) & 0xFF for i in range(DOUBLE_BIN_SIZE))


def expect_sparse(block_size):
    size = SPARSE_BIN_BLOCKS * block_size
    return bytes(0 if (i // block_size) == SPARSE_HOLE_BLOCK else (i * 17 + 3) & 0xFF
                 for i in range(size))


# guest dump layout: files concatenated in this exact order
DUMP_ORDER = ["/HELLO.TXT", "/SUB/NESTED.TXT", "/BIG.BIN", "/SPARSE.BIN", "/DOUBLE.BIN"]


class Report(object):
    def __init__(self):
        self.rows = []

    def check(self, name, ok, detail=""):
        self.rows.append((bool(ok), name, detail))
        return bool(ok)

    def count(self, ok):
        return sum(1 for r in self.rows if r[0] == ok)

    def print(self):
        for ok, name, detail in self.rows:
            line = "[ok]   %s" % name
            if detail:
                line += " -- %s" % detail
            if not ok:
                line = "[FAIL] %s" % name
                if detail:
                    line += " -- %s" % detail
            print(line)
        total = len(self.rows)
        nok = self.count(True)
        nfail = total - nok
        print("checks=%d ok=%d fail=%d" % (total, nok, nfail))
        print("=== RESULT: %s ===" % ("PASS" if nfail == 0 else "FAIL"))
        return 0 if nfail == 0 else 1


class Image(object):
    """Minimal independent ext2 reader (rev0/rev1, no extents, no journal)."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.raw = f.read()
        self.path = path
        self.sb = self._superblock()
        self.bs = self.sb["block_size"]
        self.ptrs = self.bs // 4
        self.groups = (self.sb["blocks_count"] + self.sb["blocks_per_group"] - 1) // \
            self.sb["blocks_per_group"]
        self.gdt = [self._group_desc(g) for g in range(self.groups)]

    # ---------------- metadata ----------------
    def _superblock(self):
        if len(self.raw) < 2048:
            raise ValueError("image smaller than one superblock")
        sb = self.raw[1024:2048]
        f = {}
        f["inodes_count"] = struct.unpack_from("<I", sb, 0)[0]
        f["blocks_count"] = struct.unpack_from("<I", sb, 4)[0]
        f["r_blocks_count"] = struct.unpack_from("<I", sb, 8)[0]
        f["free_blocks_count"] = struct.unpack_from("<I", sb, 12)[0]
        f["free_inodes_count"] = struct.unpack_from("<I", sb, 16)[0]
        f["first_data_block"] = struct.unpack_from("<I", sb, 20)[0]
        f["log_block_size"] = struct.unpack_from("<I", sb, 24)[0]
        f["blocks_per_group"] = struct.unpack_from("<I", sb, 32)[0]
        f["inodes_per_group"] = struct.unpack_from("<I", sb, 40)[0]
        f["magic"] = struct.unpack_from("<H", sb, 56)[0]
        f["state"] = struct.unpack_from("<H", sb, 58)[0]
        f["errors"] = struct.unpack_from("<H", sb, 60)[0]
        f["rev_level"] = struct.unpack_from("<I", sb, 76)[0]
        f["first_ino"] = struct.unpack_from("<I", sb, 84)[0]
        f["inode_size"] = struct.unpack_from("<H", sb, 88)[0]
        f["feature_compat"] = struct.unpack_from("<I", sb, 92)[0]
        f["feature_incompat"] = struct.unpack_from("<I", sb, 96)[0]
        f["feature_ro_compat"] = struct.unpack_from("<I", sb, 100)[0]
        f["volume_name"] = sb[120:136].split(b"\x00")[0].decode("latin-1")
        f["block_size"] = 1024 << f["log_block_size"]
        if f["rev_level"] == 0:
            f["inode_size"] = 128
        if f["inode_size"] == 0:
            f["inode_size"] = 128
        return f

    def _group_desc(self, g):
        off = (self.sb["first_data_block"] + 1) * self.bs + g * 32
        raw = self.raw[off:off + 32]
        if len(raw) != 32:
            raise ValueError("group descriptor %d outside image" % g)
        return {
            "block_bitmap": struct.unpack_from("<I", raw, 0)[0],
            "inode_bitmap": struct.unpack_from("<I", raw, 4)[0],
            "inode_table": struct.unpack_from("<I", raw, 8)[0],
            "free_blocks_count": struct.unpack_from("<H", raw, 12)[0],
            "free_inodes_count": struct.unpack_from("<H", raw, 14)[0],
            "used_dirs_count": struct.unpack_from("<H", raw, 16)[0],
        }

    # ---------------- inodes / blocks ----------------
    def inode(self, ino):
        if ino < 1 or ino > self.sb["inodes_count"]:
            raise ValueError("inode %d out of range" % ino)
        group = (ino - 1) // self.sb["inodes_per_group"]
        index = (ino - 1) % self.sb["inodes_per_group"]
        isize = self.sb["inode_size"]
        off = self.gdt[group]["inode_table"] * self.bs + index * isize
        raw = self.raw[off:off + isize]
        if len(raw) != isize:
            raise ValueError("inode %d outside image" % ino)
        i = {}
        i["ino"] = ino
        i["group"] = group
        i["mode"] = struct.unpack_from("<H", raw, 0)[0]
        i["uid"] = struct.unpack_from("<H", raw, 2)[0]
        i["size"] = struct.unpack_from("<I", raw, 4)[0]
        i["atime"] = struct.unpack_from("<I", raw, 8)[0]
        i["ctime"] = struct.unpack_from("<I", raw, 12)[0]
        i["mtime"] = struct.unpack_from("<I", raw, 16)[0]
        i["gid"] = struct.unpack_from("<H", raw, 24)[0]
        i["links"] = struct.unpack_from("<H", raw, 26)[0]
        i["blocks"] = struct.unpack_from("<I", raw, 28)[0]
        i["block"] = [struct.unpack_from("<I", raw, 40 + k * 4)[0] for k in range(15)]
        return i

    def block(self, blk):
        if blk < 0 or blk >= self.sb["blocks_count"]:
            raise ValueError("block %d out of range" % blk)
        off = blk * self.bs
        return self.raw[off:off + self.bs]

    def resolve(self, inode, logical):
        """Logical -> physical block, or (phys, level) with level describing
        how it was reached.  physical 0 means hole."""
        blk = inode["block"]
        if logical < 12:
            return blk[logical], "direct"
        if logical < 12 + self.ptrs:
            if blk[12] == 0:
                return 0, "single"
            t = self.block(blk[12])
            return struct.unpack_from("<I", t, (logical - 12) * 4)[0], "single"
        if logical < 12 + self.ptrs + self.ptrs * self.ptrs:
            if blk[13] == 0:
                return 0, "double"
            d = self.block(blk[13])
            off = logical - 12 - self.ptrs
            idx1, idx2 = divmod(off, self.ptrs)
            child = struct.unpack_from("<I", d, idx1 * 4)[0]
            if child == 0:
                return 0, "double"
            c = self.block(child)
            return struct.unpack_from("<I", c, idx2 * 4)[0], "double"
        if blk[14] == 0:
            return 0, "triple"
        t1 = self.block(blk[14])
        off = logical - 12 - self.ptrs - self.ptrs * self.ptrs
        i1, rest = divmod(off, self.ptrs * self.ptrs)
        i2, i3 = divmod(rest, self.ptrs)
        b1 = struct.unpack_from("<I", t1, i1 * 4)[0]
        if b1 == 0:
            return 0, "triple"
        t2 = self.block(b1)
        b2 = struct.unpack_from("<I", t2, i2 * 4)[0]
        if b2 == 0:
            return 0, "triple"
        t3 = self.block(b2)
        return struct.unpack_from("<I", t3, i3 * 4)[0], "triple"

    def read(self, inode):
        out = bytearray()
        for logical in range((inode["size"] + self.bs - 1) // self.bs):
            phys, _ = self.resolve(inode, logical)
            out += self.block(phys) if phys else b"\x00" * self.bs
        return bytes(out[:inode["size"]])

    def dirents(self, inode):
        data = self.read(inode)
        out = []
        pos = 0
        while pos + 8 <= len(data):
            ino, rec_len, name_len, ftype = struct.unpack_from("<IHBB", data, pos)
            if rec_len < 8 or rec_len % 4 or pos + rec_len > len(data):
                raise ValueError("bad rec_len %d at offset %d" % (rec_len, pos))
            if name_len > rec_len - 8:
                raise ValueError("bad name_len %d at offset %d" % (name_len, pos))
            if ino:
                out.append((ino, data[pos + 8:pos + 8 + name_len], ftype))
            pos += rec_len
        return out

    def lookup(self, path):
        """Resolve an absolute volume path to an inode."""
        cur = self.inode(2)
        if not path or path == "/":
            return cur
        for part in [p for p in path.split("/") if p]:
            if (cur["mode"] & 0o170000) != 0o040000:
                raise ValueError("%s: not a directory" % part)
            entries = self.dirents(cur)
            nxt = None
            for ino, name, _ in entries:
                if name.decode("latin-1") == part:
                    nxt = ino
                    break
            if nxt is None:
                raise ValueError("not found: %s" % path)
            cur = self.inode(nxt)
        return cur

    def walk(self, inode, prefix=""):
        """Yield (path, inode) for a directory tree."""
        for ino, name, ftype in self.dirents(inode):
            nm = name.decode("latin-1")
            if nm in (".", ".."):
                continue
            child = self.inode(ino)
            path = prefix + "/" + nm
            yield path, child
            if (child["mode"] & 0o170000) == 0o040000:
                for row in self.walk(child, path):
                    yield row

    def mapped_blocks(self, inode):
        out = []
        for logical in range((inode["size"] + self.bs - 1) // self.bs):
            phys, level = self.resolve(inode, logical)
            if phys:
                out.append((logical, phys, level))
        return out


ARROW = "  "


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def read_text_log(path):
    """Read a serial log tolerantly: QEMU -serial file: writes plain bytes,
    PowerShell redirection often writes UTF-16LE with a BOM."""
    with open(path, "rb") as f:
        raw = f.read()
    if raw.startswith(b"\xff\xfe"):
        return raw[2:].decode("utf-16-le", errors="replace")
    if raw.startswith(b"\xfe\xff"):
        return raw[2:].decode("utf-16-be", errors="replace")
    if raw.startswith(b"\xef\xbb\xbf"):
        return raw[3:].decode("utf-8", errors="replace")
    if raw.count(b"\x00") > len(raw) // 4:
        return raw.decode("utf-16-le", errors="replace")
    return raw.decode("utf-8", errors="replace")


def main():
    ap = argparse.ArgumentParser(description="independent ext2 image verifier")
    ap.add_argument("image", help="ext2 image to verify")
    ap.add_argument("--guest-dump", default=None,
                    help="file the GUEST dumped (raw bytes, or hex text)")
    ap.add_argument("--guest-dump-format", default="auto", choices=("auto", "raw", "hex"))
    ap.add_argument("--guest-log", default=None,
                    help="serial log containing '[ext2] selftest PASS (...)'")
    ap.add_argument("--print-tree", action="store_true", help="list every path in the image")
    ap.add_argument("--dump-file", default=None,
                    help="write the host parser's view of this volume path to --out")
    ap.add_argument("--out", default=None, help="output for --dump-file")
    args = ap.parse_args()

    rep = Report()
    try:
        img = Image(args.image)
    except Exception as e:                                    # noqa: BLE001
        print("[FAIL] cannot parse image: %s" % e)
        print("=== RESULT: FAIL ===")
        return 1

    sb = img.sb
    print("independent ext2 verifier")
    print("image: %s (%d bytes)" % (args.image, len(img.raw)))
    print("  block_size=%d blocks=%d inodes=%d inode_size=%d groups=%d rev=%d" % (
        sb["block_size"], sb["blocks_count"], sb["inodes_count"], sb["inode_size"],
        img.groups, sb["rev_level"]))
    print("")

    # ---------------- superblock ----------------
    rep.check("superblock magic is 0xEF53", sb["magic"] == EXT2_MAGIC,
              "magic=0x%04x" % sb["magic"])
    rep.check("superblock revision is 0 or 1", sb["rev_level"] in (0, 1),
              "rev=%d" % sb["rev_level"])
    rep.check("block size is 1024/2048/4096",
              sb["block_size"] in (1024, 2048, 4096), "block_size=%d" % sb["block_size"])
    rep.check("blocks_count matches the real image size",
              sb["blocks_count"] * sb["block_size"] <= len(img.raw),
              "blocks_count*bs=%d file=%d" % (sb["blocks_count"] * sb["block_size"],
                                              len(img.raw)))
    rep.check("inode size is sane (128..block_size)",
              128 <= sb["inode_size"] <= sb["block_size"],
              "inode_size=%d" % sb["inode_size"])
    rep.check("inodes_per_group and blocks_per_group are non-zero",
              sb["inodes_per_group"] > 0 and sb["blocks_per_group"] > 0)
    rep.check("filesystem state is clean (EXT2_VALID_FS)",
              (sb["state"] & EXT2_VALID_FS) != 0 and (sb["state"] & EXT2_ERROR_FS) == 0,
              "state=0x%04x" % sb["state"])
    rep.check("no unsupported incompatible features",
              (sb["feature_incompat"] & UNSUPPORTED_INCOMPAT) == 0,
              "incompat=0x%x" % sb["feature_incompat"])
    rep.check("first_data_block matches the block size",
              sb["first_data_block"] == (1 if sb["block_size"] == 1024 else 0),
              "first_data_block=%d" % sb["first_data_block"])

    # ---------------- group descriptors ----------------
    for g, gd in enumerate(img.gdt):
        rep.check("group %d: metadata blocks are inside the volume" % g,
                  gd["block_bitmap"] < sb["blocks_count"] and
                  gd["inode_bitmap"] < sb["blocks_count"] and
                  gd["inode_table"] < sb["blocks_count"],
                  "bb=%d ib=%d it=%d" % (gd["block_bitmap"], gd["inode_bitmap"],
                                         gd["inode_table"]))
        rep.check("group %d: free counters agree with the superblock" % g,
                  gd["free_blocks_count"] == sb["free_blocks_count"] and
                  gd["free_inodes_count"] == sb["free_inodes_count"],
                  "gd=(%d,%d) sb=(%d,%d)" % (gd["free_blocks_count"],
                                             gd["free_inodes_count"],
                                             sb["free_blocks_count"],
                                             sb["free_inodes_count"]))

    # ---------------- bitmaps ----------------
    bm = img.block(img.gdt[0]["block_bitmap"])
    used_real = sum(1 for b in range(sb["blocks_count"]) if bm[b // 8] & (1 << (b % 8)))
    rep.check("block bitmap usage matches free_blocks_count",
              used_real == sb["blocks_count"] - sb["free_blocks_count"],
              "bitmap used=%d, superblock free=%d of %d" % (
                  used_real, sb["free_blocks_count"], sb["blocks_count"]))
    im = img.block(img.gdt[0]["inode_bitmap"])
    used_ino = sum(1 for i in range(sb["inodes_count"]) if im[i // 8] & (1 << (i % 8)))
    rep.check("inode bitmap usage matches free_inodes_count",
              used_ino == sb["inodes_count"] - sb["free_inodes_count"],
              "bitmap used=%d, superblock free=%d of %d" % (
                  used_ino, sb["free_inodes_count"], sb["inodes_count"]))

    # ---------------- directory tree ----------------
    root = img.inode(2)
    rep.check("inode 2 (root) is a directory", (root["mode"] & 0o170000) == 0o040000,
              "mode=0o%o" % root["mode"])
    root_entries = img.dirents(root)
    names = {n.decode("latin-1"): (i, t) for i, n, t in root_entries}
    rep.check("root '.' points at inode 2", names.get(".") == (2, FT_DIR),
              "got %r" % (names.get("."),))
    rep.check("root '..' points at inode 2", names.get("..") == (2, FT_DIR),
              "got %r" % (names.get(".."),))

    if args.print_tree:
        print("")
        print("tree:")
        print("  / (inode 2, mode 0o%o)" % root["mode"])
        for path, ino in img.walk(root):
            kind = "dir " if (ino["mode"] & 0o170000) == 0o040000 else "file"
            print("  %s %s (inode %d, %d bytes)" % (kind, path, ino["ino"], ino["size"]))
        print("")

    # ---------------- SutureOS test-image content ----------------
    wanted = {
        "/HELLO.TXT": HELLO_TXT,
        "/SUB/NESTED.TXT": NESTED_TXT,
        "/BIG.BIN": expect_big(),
        "/SPARSE.BIN": expect_sparse(sb["block_size"]),
        "/DOUBLE.BIN": expect_double(),
    }
    is_test_image = all(n in names for n in
                        ("HELLO.TXT", "BIG.BIN", "SPARSE.BIN", "DOUBLE.BIN", "SUB"))

    seen_blocks = {}
    if is_test_image:
        entry = names["SUB"]
        sub = img.inode(entry[0])
        sub_names = {n.decode("latin-1"): (i, t) for i, n, t in img.dirents(sub)}
        rep.check("SUB is a directory with '.' and '..'",
                  (sub["mode"] & 0o170000) == 0o040000 and
                  sub_names.get(".") == (entry[0], FT_DIR) and
                  sub_names.get("..") == (2, FT_DIR),
                  "mode=0o%o .=%r ..=%r" % (sub["mode"], sub_names.get("."),
                                            sub_names.get("..")))
        rep.check("SUB contains NESTED.TXT as a regular file",
                  sub_names.get("NESTED.TXT", (0, FT_UNKNOWN))[1] == FT_REG)
        rep.check("root has no stray names",
                  set(names) == {".", "..", "HELLO.TXT", "BIG.BIN", "SPARSE.BIN",
                                 "DOUBLE.BIN", "SUB"},
                  "names=%s" % sorted(names))

        for path, want in wanted.items():
            ino = img.lookup(path)
            got = img.read(ino)
            rep.check("content of %s matches the documented rule" % path,
                      got == want,
                      "%d bytes, crc32=0x%08x%s" % (
                          len(got), crc32(got),
                          "" if got == want else " EXPECTED crc32=0x%08x" % crc32(want)))
            rep.check("%s has the expected size" % path, len(got) == len(want),
                      "%d bytes (expected %d)" % (len(got), len(want)))
            for logical, phys, _ in img.mapped_blocks(ino):
                if phys >= sb["blocks_count"]:
                    rep.check("%s: block %d is inside the volume" % (path, phys), False)
                if phys in seen_blocks:
                    rep.check("%s: data block %d is not shared" % (path, phys), False,
                              "also used by %s" % seen_blocks[phys])
                seen_blocks[phys] = path

        # ---- indirect traversal shape ----
        big = img.inode(img.lookup("/BIG.BIN")["ino"])
        nb = (big["size"] + sb["block_size"] - 1) // sb["block_size"]
        if nb > 12:
            rep.check("BIG.BIN uses i_block[12] (single indirect)",
                      big["block"][12] != 0, "i_block[12]=%d" % big["block"][12])
            table = img.block(big["block"][12])
            indirect_ptrs = [struct.unpack_from("<I", table, k * 4)[0]
                             for k in range(sb["block_size"] // 4)]
            expect = []
            for lb in range(12, nb):
                phys, level = img.resolve(big, lb)
                expect.append((lb, phys, level))
            ok = all(level == "single" for _, _, level in expect) and \
                all(indirect_ptrs[lb - 12] == phys for lb, phys, _ in expect)
            rep.check("BIG.BIN logical blocks 12..%d resolve through the indirect table"
                      % (nb - 1), ok,
                      "%d blocks via i_block[12]=%d" % (len(expect), big["block"][12]))
            rep.check("BIG.BIN uses no double/triple indirect",
                      big["block"][13] == 0 and big["block"][14] == 0)
        dbl = img.inode(img.lookup("/DOUBLE.BIN")["ino"])
        ndb = (dbl["size"] + sb["block_size"] - 1) // sb["block_size"]
        if ndb > 12 + sb["block_size"] // 4:
            rep.check("DOUBLE.BIN uses i_block[13] (double indirect)",
                      dbl["block"][13] != 0, "i_block[13]=%d" % dbl["block"][13])
            levels = set(level for _, _, level in img.mapped_blocks(dbl))
            rep.check("DOUBLE.BIN walks the double-indirect tree",
                      "double" in levels and levels <= {"direct", "single", "double"},
                      "levels=%s" % sorted(levels))
            d = img.block(dbl["block"][13])
            used_children = set()
            for lb in range(12 + sb["block_size"] // 4, ndb):
                off = lb - 12 - sb["block_size"] // 4
                idx1, idx2 = divmod(off, sb["block_size"] // 4)
                child = struct.unpack_from("<I", d, idx1 * 4)[0]
                used_children.add((idx1, child))
                c = img.block(child)
                phys = struct.unpack_from("<I", c, idx2 * 4)[0]
                if phys == 0:
                    rep.check("DOUBLE.BIN logical block %d is mapped" % lb, False)
            rep.check("DOUBLE.BIN double-indirect children are all in range",
                      all(0 < c < sb["blocks_count"] for _, c in used_children),
                      "%d children" % len(used_children))
        sparse = img.inode(img.lookup("/SPARSE.BIN")["ino"])
        rep.check("SPARSE.BIN hole pointer (i_block[%d]) is 0" % SPARSE_HOLE_BLOCK,
                  sparse["block"][SPARSE_HOLE_BLOCK] == 0)
        hole = img.read(sparse)[SPARSE_HOLE_BLOCK * sb["block_size"]:
                                (SPARSE_HOLE_BLOCK + 1) * sb["block_size"]]
        rep.check("SPARSE.BIN hole reads back as zeros",
                  hole == b"\x00" * sb["block_size"])
        rep.check("SPARSE.BIN neighbours are mapped",
                  sparse["block"][0] != 0 and sparse["block"][2] != 0)
    else:
        print("[--] this image does not contain the SutureOS test files; "
              "content checks skipped (structure-only mode)")

    # ---------------- guest dump ----------------
    if args.guest_dump:
        with open(args.guest_dump, "rb") as f:
            dump = f.read()
        fmt = args.guest_dump_format
        if fmt == "auto":
            try:
                text = dump.decode("ascii")
                stripped = re.sub(r"\s+", "", text)
                fmt = "hex" if stripped and re.fullmatch(r"[0-9a-fA-F]+", stripped) \
                    and len(stripped) % 2 == 0 else "raw"
            except UnicodeDecodeError:
                fmt = "raw"
        if fmt == "hex":
            dump = bytes.fromhex(re.sub(r"\s+", "", dump.decode("ascii")))
        print("guest dump: %s (%d bytes, %s)" % (args.guest_dump, len(dump), fmt))
        if not is_test_image:
            rep.check("guest dump can be compared (image is a SutureOS test image)",
                      False, "cannot build the expectation")
        else:
            off = 0
            for path in DUMP_ORDER:
                want = wanted[path]
                got = dump[off:off + len(want)]
                rep.check("guest dump segment %s equals the host expectation" % path,
                          got == want,
                          "guest crc32=0x%08x host crc32=0x%08x (%d bytes)" % (
                              crc32(got), crc32(want), len(want)))
                off += len(want)
            rep.check("guest dump has no trailing bytes", off == len(dump),
                      "expected %d bytes, dump has %d" % (off, len(dump)))

    # ---------------- guest serial log ----------------
    if args.guest_log:
        log = read_text_log(args.guest_log)
        print("guest log: %s (%d bytes)" % (args.guest_log, len(log)))
        m = re.search(r"\[ext2\] selftest PASS \(([^)]*)\)", log)
        rep.check("serial log contains '[ext2] selftest PASS (...)'", m is not None)
        if re.search(r"\[ext2\] selftest FAIL", log):
            rep.check("serial log contains no '[ext2] selftest FAIL'", False)
        if m and is_test_image:
            pairs = dict(re.findall(r"(\w+)=0x([0-9a-fA-F]+)", m.group(1)))
            for key, path in (("hello", "/HELLO.TXT"), ("nested", "/SUB/NESTED.TXT"),
                              ("big", "/BIG.BIN"), ("sparse", "/SPARSE.BIN"),
                              ("double", "/DOUBLE.BIN")):
                host = crc32(wanted[path])
                guest = int(pairs.get(key, "0"), 16)
                rep.check("guest CRC32 %s == host CRC32 of %s" % (key, path),
                          guest == host,
                          "guest=0x%08x host=0x%08x" % (guest, host))
            mount = re.search(r"\[ext2\] mount blocksz=(\d+) blocks=(\d+) inodes=(\d+) "
                              r"groups=(\d+) rev=(\d+)", log)
            rep.check("serial log has the ext2 mount line", mount is not None,
                      mount.group(0) if mount else "not found")
            if mount:
                rep.check("mount line block size matches the image",
                          int(mount.group(1)) == sb["block_size"],
                          "guest=%s image=%d" % (mount.group(1), sb["block_size"]))
                rep.check("mount line block count matches the image",
                          int(mount.group(2)) == sb["blocks_count"],
                          "guest=%s image=%d" % (mount.group(2), sb["blocks_count"]))

    # ---------------- optional single-file dump ----------------
    if args.dump_file:
        try:
            data = img.read(img.lookup(args.dump_file))
        except Exception as e:                                # noqa: BLE001
            rep.check("dump %s" % args.dump_file, False, str(e))
        else:
            out = args.out or (os.path.basename(args.dump_file.strip("/")) + ".host")
            with open(out, "wb") as f:
                f.write(data)
            print("host view of %s -> %s (%d bytes, crc32=0x%08x)" % (
                args.dump_file, out, len(data), crc32(data)))
            rep.check("host dump of %s written" % args.dump_file, True, out)

    print("")
    return rep.print()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as exc:
        print("[FAIL] %s" % exc)
        print("=== RESULT: FAIL ===")
        sys.exit(1)
