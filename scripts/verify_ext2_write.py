#!/usr/bin/env python3
"""Independent host-side verifier for the ext2 WRITE path (SutureOS B3 batch).

It does NOT trust anything the guest printed: it re-parses the dirty image with
a standalone ext2 reader (fixed on-disk offsets, its own block-pointer walk) and
checks the invariants that a broken write path would violate:

  1. superblock / group descriptor free counters vs the real bitmaps
       - every group's bg_free_blocks_count / bg_free_inodes_count
       - the sum over groups vs s_free_blocks_count / s_free_inodes_count
       - every in-use inode's bitmap bit, every referenced block's bitmap bit
  2. the write self-test artifact /WRT_KEEP.BIN
       - i_size, i_blocks (512-byte units), the 12 direct pointers, the
         single-indirect block and (when the volume was big enough for the
         guest to grow it) the double-indirect tree, plus the byte content
         (pattern (i*41+5) & 0xFF)
  3. directory chain integrity
       - every directory parses, "." points to itself, ".." to the real parent
       - no block referenced by two different inodes (or twice by one inode)
       - no orphan inodes (anything in use above the reserved 1..10 must be
         reachable from the root)
       - each directory's i_links_count == 2 + number of subdirectories
       - bg_used_dirs_count matches the directories really present per group
  4. the self-test really cleaned up: /WRTDIR, /WRTDIR2 and /M13K.BIN are gone
  5. with --clean <pristine image from scripts/mkfs_ext2_testimg.py>:
       - the five fixture files are byte-identical to the pristine image
         (proves the write path never clobbered existing data)
       - free block / free inode deltas equal exactly what the artifact uses
  6. with --guest-log <serial log>: the guest printed the PASS line plus
       "[ext2] bitmaps consistent (free blocks/inodes)"

Usage:
  py -3 scripts/mkfs_ext2_testimg.py                 # (re)generate the images
  py -3 scripts/verify_ext2_write.py build/ext2.img --clean build/ext2_clean.img \
      --guest-log build/serial_ext2_1k.log
  py -3 scripts/verify_ext2_write.py build/ext2_4k.img --clean build/ext2_4k_clean.img

The script never writes to the image; it only reads.
"""

import argparse
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

EXT2_SUPER_MAGIC = 0xEF53
S_IFMT = 0o170000
S_IFDIR = 0o040000
S_IFREG = 0o100000
RESERVED_INODES = range(1, 11)          # inodes 1..10 are reserved (root is 2)

# ---- content rules shared with kernel/fs/ext2.c and mkfs_ext2_testimg.py ----
HELLO_TXT = b"Hello from SutureOS ext2!\nSecond line: read via the real ext2 driver.\n"
NESTED_TXT = b"Nested file: SUB/NESTED.TXT reached through ext2 directory traversal.\n"
BIG_BIN_SIZE = 64 * 1024
DOUBLE_BIN_SIZE = 300 * 1024
SPARSE_BLOCKS = 3
SPARSE_HOLE_BLOCK = 1
WRT_BLOCKS = 13                          # 13 logical blocks: 12 direct + 1 indirect
KEEP_NAME = "/WRT_KEEP.BIN"
PASS_LINE = ("[ext2] write self-test PASS (create=1 write=1 readback=1 indirect=1 "
             "rename=1 unlink=1 rmdir=1)")
BITMAP_LINE = "[ext2] bitmaps consistent (free blocks/inodes)"


def big_byte(i):
    return (i * 31 + 7) & 0xFF


def double_byte(i):
    return (i * 29 + 11) & 0xFF


def sparse_byte(i, block_size):
    if (i // block_size) == SPARSE_HOLE_BLOCK:
        return 0
    return (i * 17 + 3) & 0xFF


def keep_byte(i):
    return (i * 41 + 5) & 0xFF


class Report(object):
    def __init__(self):
        self.rows = []

    def check(self, name, ok, detail=""):
        self.rows.append((name, bool(ok), detail))

    def print(self):
        nfail = 0
        for name, ok, detail in self.rows:
            mark = "ok  " if ok else "FAIL"
            line = "  [%s] %s" % (mark, name)
            if detail:
                line += "  (%s)" % detail
            print(line)
            if not ok:
                nfail += 1
        nok = len(self.rows) - nfail
        print("")
        print("host-side ext2 write verification: checks=%d ok=%d fail=%d"
              % (len(self.rows), nok, nfail))
        print("RESULT: %s" % ("PASS" if nfail == 0 else "FAIL"))
        return 0 if nfail == 0 else 1


class Image(object):
    """Minimal independent ext2 reader (rev0/rev1, classic block pointers)."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.raw = f.read()
        self.path = path
        self.sb = self._superblock()
        self.bs = self.sb["block_size"]
        self.ptrs = self.bs // 4
        self.groups = ((self.sb["blocks_count"] - self.sb["first_data_block"])
                       + self.sb["blocks_per_group"] - 1) // self.sb["blocks_per_group"]
        self.gdt = [self._group_desc(g) for g in range(self.groups)]

    # ---------------- metadata ----------------
    def _superblock(self):
        if len(self.raw) < 2048:
            raise ValueError("image smaller than one superblock")
        sb = self.raw[1024:2048]
        f = {}
        f["inodes_count"] = struct.unpack_from("<I", sb, 0)[0]
        f["blocks_count"] = struct.unpack_from("<I", sb, 4)[0]
        f["free_blocks_count"] = struct.unpack_from("<I", sb, 12)[0]
        f["free_inodes_count"] = struct.unpack_from("<I", sb, 16)[0]
        f["first_data_block"] = struct.unpack_from("<I", sb, 20)[0]
        f["log_block_size"] = struct.unpack_from("<I", sb, 24)[0]
        f["blocks_per_group"] = struct.unpack_from("<I", sb, 32)[0]
        f["inodes_per_group"] = struct.unpack_from("<I", sb, 40)[0]
        f["magic"] = struct.unpack_from("<H", sb, 56)[0]
        f["state"] = struct.unpack_from("<H", sb, 58)[0]
        f["rev_level"] = struct.unpack_from("<I", sb, 76)[0]
        f["first_ino"] = struct.unpack_from("<I", sb, 84)[0]
        f["inode_size"] = struct.unpack_from("<H", sb, 88)[0]
        f["feature_incompat"] = struct.unpack_from("<I", sb, 96)[0]
        f["block_size"] = 1024 << f["log_block_size"]
        if f["rev_level"] == 0 or f["inode_size"] == 0:
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

    def group_block_limit(self, g):
        first = self.sb["first_data_block"] + g * self.sb["blocks_per_group"]
        left = max(0, self.sb["blocks_count"] - first)
        return min(left, self.sb["blocks_per_group"])

    def group_inode_limit(self, g):
        first = g * self.sb["inodes_per_group"]
        left = max(0, self.sb["inodes_count"] - first)
        return min(left, self.sb["inodes_per_group"])

    # ---------------- bitmaps ----------------
    def block_bitmap(self, g):
        return self.block(self.gdt[g]["block_bitmap"])

    def inode_bitmap(self, g):
        return self.block(self.gdt[g]["inode_bitmap"])

    def block_used(self, blk):
        g = (blk - self.sb["first_data_block"]) // self.sb["blocks_per_group"]
        bit = (blk - self.sb["first_data_block"]) % self.sb["blocks_per_group"]
        return bool(self.block_bitmap(g)[bit // 8] & (1 << (bit % 8)))

    def inode_used(self, ino):
        g = (ino - 1) // self.sb["inodes_per_group"]
        bit = (ino - 1) % self.sb["inodes_per_group"]
        return bool(self.inode_bitmap(g)[bit // 8] & (1 << (bit % 8)))

    def measured_free(self):
        """(free blocks, free inodes, per-group free blocks, per-group inodes)."""
        fb = 0
        fi = 0
        per_b = []
        per_i = []
        for g in range(self.groups):
            bm = self.block_bitmap(g)
            lim_b = self.group_block_limit(g)
            free_b = 0
            for i in range(lim_b):
                if not (bm[i // 8] & (1 << (i % 8))):
                    free_b += 1
            im = self.inode_bitmap(g)
            lim_i = self.group_inode_limit(g)
            free_i = 0
            for i in range(lim_i):
                if not (im[i // 8] & (1 << (i % 8))):
                    free_i += 1
            per_b.append(free_b)
            per_i.append(free_i)
            fb += free_b
            fi += free_i
        return fb, fi, per_b, per_i

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
        i["size"] = struct.unpack_from("<I", raw, 4)[0]
        i["links"] = struct.unpack_from("<H", raw, 26)[0]
        i["blocks"] = struct.unpack_from("<I", raw, 28)[0]
        i["block"] = [struct.unpack_from("<I", raw, 40 + k * 4)[0] for k in range(15)]
        return i

    def block(self, blk):
        if blk <= 0 or blk >= self.sb["blocks_count"]:
            raise ValueError("block %d out of range" % blk)
        off = blk * self.bs
        return self.raw[off:off + self.bs]

    def resolve(self, inode, logical):
        """Logical -> (physical block, level); physical 0 means hole."""
        blk = inode["block"]
        if logical < 12:
            return blk[logical], "direct"
        if logical < 12 + self.ptrs:
            if blk[12] == 0:
                return 0, "single"
            t = self.block(blk[12])
            return struct.unpack_from("<I", t, (logical - 12) * 4)[0], "single"
        if logical < 12 + self.ptrs + self.ptrs ** 2:
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
        off = logical - 12 - self.ptrs - self.ptrs ** 2
        i1, rest = divmod(off, self.ptrs ** 2)
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

    def blocks_of(self, inode):
        """Every physical block this inode owns: data blocks + indirect tables."""
        out = []
        blk = inode["block"]
        for logical in range(12):
            if blk[logical]:
                out.append((blk[logical], "data/direct"))
        if blk[12]:
            out.append((blk[12], "table/single"))
            t = self.block(blk[12])
            for i in range(self.ptrs):
                v = struct.unpack_from("<I", t, i * 4)[0]
                if v:
                    out.append((v, "data/single"))
        if blk[13]:
            out.append((blk[13], "table/double"))
            d = self.block(blk[13])
            for i in range(self.ptrs):
                child = struct.unpack_from("<I", d, i * 4)[0]
                if child:
                    out.append((child, "table/double"))
                    c = self.block(child)
                    for j in range(self.ptrs):
                        v = struct.unpack_from("<I", c, j * 4)[0]
                        if v:
                            out.append((v, "data/double"))
        if blk[14]:
            out.append((blk[14], "table/triple"))
        return out

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
                out.append((ino, data[pos + 8:pos + 8 + name_len].decode("latin-1"),
                            ftype))
            pos += rec_len
        return out

    def lookup(self, path):
        cur = self.inode(2)
        if not path or path == "/":
            return cur
        for part in [p for p in path.split("/") if p]:
            if (cur["mode"] & S_IFMT) != S_IFDIR:
                raise ValueError("%s: not a directory" % part)
            nxt = None
            for ino, name, _ in self.dirents(cur):
                if name == part:
                    nxt = ino
                    break
            if nxt is None:
                raise ValueError("not found: %s" % path)
            cur = self.inode(nxt)
        return cur

    def walk(self, inode, prefix="", seen=None):
        """Yield (path, inode, parent_inode) for the whole tree below `inode`."""
        for ino, name, _ in self.dirents(inode):
            if name in (".", ".."):
                continue
            child = self.inode(ino)
            path = prefix + "/" + name
            yield path, child, inode
            if (child["mode"] & S_IFMT) == S_IFDIR:
                for row in self.walk(child, path):
                    yield row


def table_count(img, inode):
    """Number of indirect table blocks this inode owns (single, double, triple)."""
    n = 0
    blk = inode["block"]
    if blk[12]:
        n += 1
    if blk[13]:
        n += 1
        d = img.block(blk[13])
        for i in range(img.ptrs):
            if struct.unpack_from("<I", d, i * 4)[0]:
                n += 1
    if blk[14]:
        n += 1
    return n


def read_text_log(path):
    with open(path, "rb") as f:
        raw = f.read()
    if raw.startswith(b"\xff\xfe"):
        return raw[2:].decode("utf-16-le", errors="replace")
    if raw.startswith(b"\xef\xbb\xbf"):
        return raw[3:].decode("utf-8", errors="replace")
    try:
        return raw.decode("utf-8", errors="replace")
    except UnicodeDecodeError:
        return raw.decode("latin-1", errors="replace")


def verify(dirty_path, clean_path, guest_log, report):
    img = Image(dirty_path)
    sb = img.sb
    bs = img.bs

    # ---- 1. basic geometry (a wrong geometry would poison everything else) ----
    report.check("superblock magic 0xEF53", sb["magic"] == EXT2_SUPER_MAGIC,
                 "0x%04x" % sb["magic"])
    report.check("revision is 0 or 1", sb["rev_level"] in (0, 1), "rev=%d" % sb["rev_level"])
    report.check("block size is 1024/2048/4096", bs in (1024, 2048, 4096), "%d" % bs)
    report.check("inode size divides the block size", bs % sb["inode_size"] == 0,
                 "inode_size=%d" % sb["inode_size"])

    # ---- 2. free counters vs bitmaps -------------------------------------
    fb, fi, per_b, per_i = img.measured_free()
    report.check("superblock free blocks == bitmap", sb["free_blocks_count"] == fb,
                 "superblock=%d measured=%d" % (sb["free_blocks_count"], fb))
    report.check("superblock free inodes == bitmap", sb["free_inodes_count"] == fi,
                 "superblock=%d measured=%d" % (sb["free_inodes_count"], fi))
    gd_b_ok = all(img.gdt[g]["free_blocks_count"] == per_b[g] for g in range(img.groups))
    gd_i_ok = all(img.gdt[g]["free_inodes_count"] == per_i[g] for g in range(img.groups))
    report.check("group descriptor free blocks == bitmap", gd_b_ok,
                 "; ".join("g%d gd=%d bm=%d" % (g, img.gdt[g]["free_blocks_count"], per_b[g])
                           for g in range(img.groups)))
    report.check("group descriptor free inodes == bitmap", gd_i_ok,
                 "; ".join("g%d gd=%d bm=%d" % (g, img.gdt[g]["free_inodes_count"], per_i[g])
                           for g in range(img.groups)))
    report.check("sum of group free blocks == superblock",
                 sum(img.gdt[g]["free_blocks_count"] for g in range(img.groups))
                 == sb["free_blocks_count"])
    report.check("sum of group free inodes == superblock",
                 sum(img.gdt[g]["free_inodes_count"] for g in range(img.groups))
                 == sb["free_inodes_count"])

    # ---- 3. directory chain: tree walk, "."/"..", links, dedup, orphans ----
    tree = list(img.walk(img.inode(2)))
    root = img.inode(2)
    dirs = {2: root}
    parents = {}
    entries_by_parent = {}
    walk_errors = []
    for path, inode, parent in tree:
        if inode["ino"] in dirs and (inode["mode"] & S_IFMT) == S_IFDIR:
            walk_errors.append("%s: directory visited twice (loop)" % path)
        if (inode["mode"] & S_IFMT) == S_IFDIR:
            dirs[inode["ino"]] = inode
        parents.setdefault(inode["ino"], parent["ino"])
        entries_by_parent.setdefault(parent["ino"], []).append((path, inode))
    report.check("directory tree walks without loops", not walk_errors,
                 "; ".join(walk_errors[:3]))

    # "." and ".." of every directory
    dot_ok = True
    dotdot_ok = True
    dot_detail = []
    for ino, d in sorted(dirs.items()):
        try:
            ents = img.dirents(d)
        except ValueError as exc:
            dot_ok = False
            dotdot_ok = False
            dot_detail.append("inode %d: %s" % (ino, exc))
            continue
        dot = [e for e in ents if e[1] == "."]
        dotdot = [e for e in ents if e[1] == ".."]
        expect_parent = 2 if ino == 2 else parents.get(ino, 2)
        if len(dot) != 1 or dot[0][0] != ino:
            dot_ok = False
            dot_detail.append("inode %d: bad '.'" % ino)
        if len(dotdot) != 1 or dotdot[0][0] != expect_parent:
            dotdot_ok = False
            dot_detail.append("inode %d: '..' -> %s (want %d)"
                              % (ino, dotdot[0][0] if dotdot else "none", expect_parent))
    report.check("every directory has '.' -> itself", dot_ok, "; ".join(dot_detail[:3]))
    report.check("every directory has '..' -> real parent", dotdot_ok, "; ".join(dot_detail[:3]))

    # parent link counts: 2 + number of subdirectories
    link_ok = True
    link_detail = []
    for ino, d in sorted(dirs.items()):
        subdirs = sum(1 for _, child, _ in tree
                      if parents.get(child["ino"]) == ino
                      and (child["mode"] & S_IFMT) == S_IFDIR)
        want = 2 + subdirs
        if d["links"] != want:
            link_ok = False
            link_detail.append("dir inode %d links=%d want=%d" % (ino, d["links"], want))
    report.check("directory i_links_count == 2 + subdirectories", link_ok,
                 "; ".join(link_detail[:4]))

    # no duplicate blocks, every referenced block marked used, no orphan inodes
    owner = {}
    dup = []
    not_marked = []
    file_inodes = {}
    for ino, d in sorted(dirs.items()):
        file_inodes[ino] = d
    for path, inode, _ in tree:
        file_inodes.setdefault(inode["ino"], inode)
    for ino, inode in sorted(file_inodes.items()):
        try:
            for blk, kind in img.blocks_of(inode):
                if blk in owner:
                    dup.append("block %d: inode %d (%s) and inode %d"
                               % (blk, owner[blk], kind, ino))
                else:
                    owner[blk] = ino
                if not img.block_used(blk):
                    not_marked.append("block %d (inode %d) not marked used" % (blk, ino))
        except ValueError as exc:
            not_marked.append("inode %d: %s" % (ino, exc))
    report.check("no block referenced twice", not dup, "; ".join(dup[:4]))
    report.check("every referenced block is marked used", not not_marked,
                 "; ".join(not_marked[:4]))

    used_inodes = [i for i in range(1, sb["inodes_count"] + 1) if img.inode_used(i)]
    reachable = set(file_inodes.keys())
    orphans = [i for i in used_inodes
               if i not in reachable and i not in RESERVED_INODES]
    report.check("no orphan inodes (every used inode is reachable)", not orphans,
                 "orphans=%s" % orphans[:8])
    unmarked_inodes = [i for i in reachable if not img.inode_used(i)]
    report.check("every reachable inode is marked used", not unmarked_inodes,
                 "unmarked=%s" % unmarked_inodes[:8])

    # used_dirs_count per group
    dirs_per_group = {}
    for ino in dirs:
        g = (ino - 1) // sb["inodes_per_group"]
        dirs_per_group[g] = dirs_per_group.get(g, 0) + 1
    used_dirs_ok = True
    used_dirs_detail = []
    for g in range(img.groups):
        want = dirs_per_group.get(g, 0)
        got = img.gdt[g]["used_dirs_count"]
        if got != want:
            used_dirs_ok = False
            used_dirs_detail.append("g%d gd=%d actual=%d" % (g, got, want))
    report.check("group used_dirs_count == directories present", used_dirs_ok,
                 "; ".join(used_dirs_detail))

    # ---- 4. the write self-test cleaned up after itself -------------------
    leftovers = []
    for path in ("/WRTDIR", "/WRTDIR2", "/M13K.BIN", "/WRTDIR/F13K.BIN",
                 "/VFSPROBE.BIN", "/VFSDIR", "/VFSDIR/MOVED.BIN"):
        try:
            img.lookup(path)
            leftovers.append(path)
        except ValueError:
            pass
    report.check("self-test leftovers removed", not leftovers,
                 "still present: %s" % leftovers)

    # ---- 5. the persistent artifact: inode metadata + byte content -------
    keep = None
    try:
        keep = img.lookup(KEEP_NAME)
    except ValueError as exc:
        report.check("artifact %s exists" % KEEP_NAME, False, str(exc))
    if keep:
        ptrs = img.ptrs
        single_end = 12 + ptrs
        double_end = single_end + ptrs * ptrs
        size = keep["size"]
        nblocks = size // bs if bs else 0
        allowed = (WRT_BLOCKS, single_end + 8)          # small or double-indirect
        report.check("artifact is a regular file",
                     (keep["mode"] & S_IFMT) == S_IFREG,
                     "mode=0%o" % keep["mode"])
        report.check("artifact size is a whole number of blocks", size % bs == 0,
                     "size=%d bs=%d" % (size, bs))
        report.check("artifact block count is 13 or %d (double indirect)" % allowed[1],
                     nblocks in allowed, "nblocks=%d" % nblocks)
        report.check("artifact uses the 12 direct pointers",
                     all(keep["block"][i] != 0 for i in range(12)))
        report.check("artifact has a single-indirect block", keep["block"][12] != 0,
                     "i_block[12]=%d" % keep["block"][12])
        logical12, level12 = img.resolve(keep, 12)
        report.check("logical block 12 resolves through the single indirect",
                     logical12 != 0 and level12 == "single",
                     "phys=%d level=%s" % (logical12, level12))
        if nblocks > single_end:
            logical_d, level_d = img.resolve(keep, single_end)
            report.check("artifact has a double-indirect tree (block %d)" % single_end,
                         keep["block"][13] != 0 and logical_d != 0 and level_d == "double",
                         "i_block[13]=%d phys=%d level=%s"
                         % (keep["block"][13], logical_d, level_d))
        elif nblocks <= double_end:
            report.check("artifact has no double-indirect tree when it is not needed",
                         keep["block"][13] == 0, "i_block[13]=%d" % keep["block"][13])
        tables = table_count(img, keep)
        want_iblocks = (nblocks + tables) * (bs // 512)
        report.check("artifact i_blocks == %d (%d data + %d table blocks)"
                     % (want_iblocks, nblocks, tables),
                     keep["blocks"] == want_iblocks, "i_blocks=%d" % keep["blocks"])
        # every data block must be distinct, non-zero and inside the volume
        dat = []
        ok_resolve = True
        for b in range(nblocks):
            phys, _lvl = img.resolve(keep, b)
            if phys == 0:
                ok_resolve = False
                break
            dat.append(phys)
        report.check("artifact data blocks all resolve to non-zero blocks", ok_resolve)
        report.check("artifact data blocks are all distinct",
                     ok_resolve and len(set(dat)) == nblocks,
                     "%d blocks, %d distinct" % (len(dat), len(set(dat))))
        content = img.read(keep)
        want = bytes(keep_byte(i) for i in range(size))
        if content == want:
            report.check("artifact content byte-exact (pattern (i*41+5)&0xFF)", True)
        else:
            bad = next((i for i in range(min(len(content), len(want)))
                        if content[i] != want[i]), None)
            report.check("artifact content byte-exact (pattern (i*41+5)&0xFF)", False,
                         "first mismatch at byte %s (len %d, want %d)"
                         % (bad, len(content), len(want)))

    # ---- 6. pristine-image comparison -----------------------------------
    if clean_path:
        clean = Image(clean_path)
        report.check("clean image uses the same block size", clean.bs == bs,
                     "clean=%d dirty=%d" % (clean.bs, bs))
        if clean.bs == bs:
            fixtures = [
                ("/HELLO.TXT", HELLO_TXT),
                ("/SUB/NESTED.TXT", NESTED_TXT),
                ("/BIG.BIN", bytes(big_byte(i) for i in range(BIG_BIN_SIZE))),
                ("/SPARSE.BIN", bytes(sparse_byte(i, bs)
                                      for i in range(SPARSE_BLOCKS * bs))),
                ("/DOUBLE.BIN", bytes(double_byte(i) for i in range(DOUBLE_BIN_SIZE))),
            ]
            for path, want in fixtures:
                try:
                    got_in = img.lookup(path)
                    got = img.read(got_in)
                    c_in = clean.lookup(path)
                    ref = clean.read(c_in)
                    ok = (got == want) and (got == ref)
                except ValueError as exc:
                    ok = False
                    got = b""
                    ref = b""
                report.check("pristine file %s unchanged" % path, ok,
                             "dirty=%d bytes clean=%d bytes" % (len(got), len(ref)))
            # free-count deltas must equal exactly what the artifact uses
            cfb, cfi, _, _ = clean.measured_free()
            if keep:
                used_blocks = len(keep["block"]) and len([b for b in img.blocks_of(keep)])
                report.check("free block delta == artifact blocks",
                             cfb - fb == used_blocks,
                             "clean=%d dirty=%d artifact_blocks=%d"
                             % (cfb, fb, used_blocks))
            report.check("free inode delta == 1 (the artifact inode)",
                         cfi - fi == 1, "clean=%d dirty=%d" % (cfi, fi))
            # the root directory may have grown (a freed directory block is not
            # returned to the bitmap by design); it must never have shrunk.
            report.check("root directory did not shrink",
                         img.inode(2)["size"] >= clean.inode(2)["size"],
                         "dirty=%d clean=%d" % (img.inode(2)["size"],
                                                clean.inode(2)["size"]))

    # ---- 7. guest log (optional) ----------------------------------------
    if guest_log:
        text = read_text_log(guest_log)
        report.check("guest log contains the write self-test PASS line",
                     PASS_LINE in text)
        report.check("guest log contains the bitmap consistency line",
                     BITMAP_LINE in text)
        report.check("guest log contains the VFS-layer write self-test PASS line",
                     "[ext2] vfs write self-test PASS (" in text)
        report.check("guest log reports no ext2 write self-test FAIL",
                     "write self-test FAIL" not in text)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dirty", nargs="?", default=os.path.join(ROOT, "build", "ext2.img"),
                    help="image written by the guest (default build/ext2.img)")
    ap.add_argument("--clean", default=None,
                    help="pristine image from scripts/mkfs_ext2_testimg.py "
                         "(default: <dirty>.clean.img or build/ext2_clean.img heuristic)")
    ap.add_argument("--guest-log", default=None,
                    help="serial log from the QEMU run")
    args = ap.parse_args()

    if not os.path.exists(args.dirty):
        print("image not found: %s" % args.dirty)
        return 1

    clean = args.clean
    if clean is None:
        base = os.path.basename(args.dirty).replace(".img", "")
        candidate = os.path.join(os.path.dirname(os.path.abspath(args.dirty)),
                                 base + "_clean.img")
        if os.path.exists(candidate):
            clean = candidate
    if clean and not os.path.exists(clean):
        print("clean image not found: %s" % clean)
        return 1

    print("dirty image : %s" % os.path.abspath(args.dirty))
    print("clean image : %s" % (os.path.abspath(clean) if clean else "(not given)"))
    print("guest log   : %s" % (args.guest_log or "(not given)"))
    print("")

    report = Report()
    try:
        verify(args.dirty, clean, args.guest_log, report)
    except (ValueError, KeyError, struct.error) as exc:
        report.check("parser survived the image", False, "%s: %s"
                     % (type(exc).__name__, exc))
    return report.print()


if __name__ == "__main__":
    sys.exit(main())
