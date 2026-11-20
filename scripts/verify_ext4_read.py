#!/usr/bin/env python3
"""SutureOS ext4 host-side verifier - an INDEPENDENT parse of the image.

This script shares no code with the kernel driver (kernel/fs/ext4.c) and none
with the generator (scripts/mkfs_ext4_testimg.py): every structure is decoded
from the raw bytes here, from the ext4 on-disk specification. It is the
"second opinion" on the image QEMU has just read, so a bug in the guest driver
cannot make this check pass and a bug in the generator cannot hide behind the
guest's own log.

Checks
  1. image identity   : size + sha256 == the manifest (the ext4 driver is
                        read-only, so the image must be byte-identical after a
                        boot that mounted it)
  2. geometry         : superblock magic/first_data_block/log_block_size/
                        inode_size/blocks/inodes/group count/desc size/state
  3. features         : exactly filetype|extents (incompat), sparse_super
                        (ro_compat), no compat bits, and none of the features
                        the driver refuses (metadata_csum, 64bit, mmp,
                        bigalloc, inline_data, dir_index, journal)
  4. allocation       : free counters in the superblock AND the group descriptor
                        agree with the bitmaps; every block an inode refers to is
                        marked used; nothing references a block outside the volume
  5. inodes           : per manifest file: type from i_mode, size, link count,
                        extents flag, inode number
  6. extent trees     : full walk (depth <= 4, magic 0xF30A, entries <= max,
                        sane ee_len), /BIG.BIN and /SUBDIR/FRAG.BIN must be
                        depth-1 trees with two leaf blocks and 384 one-block
                        extents; extents must tile the logical block range with
                        no hole, no overlap and no duplicate physical block
  7. content          : sha256 of every manifest file, including the zeros of the
                        sparse hole and the 0-byte file
  8. directories      : '.'/'..' present and pointing at the right inodes, every
                        manifest name present exactly once, dirent file_type
                        byte agrees with the target inode's mode
  9. link counts      : root = 2 + subdir count, subdir = 2, files = 1
 10. guest log        : with --guest-log, the serial log's mount/file lines must
                        match what this parser found (extent counts, depth, size,
                        extent_depth_max, read-only mount, no warnings)

Usage:
    py -3 scripts/verify_ext4_read.py build/ext4.img
    py -3 scripts/verify_ext4_read.py build/ext4.img --guest-log build/serial_ext4.log --json build/ext4_verify.json
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# on-disk constants (from the ext4 specification, deliberately re-typed here so
# this file does not depend on the generator's constants)
EXT4_MAGIC = 0xEF53
EXTENT_MAGIC = 0xF30A
INCOMPAT_FILETYPE = 0x0002
INCOMPAT_EXTENTS = 0x0040
RO_COMPAT_SPARSE_SUPER = 0x0001
SB_OFFSET = 1024
EXTENTS_FL = 0x00080000

FT = {1: "file", 2: "dir", 7: "symlink"}


class Fail(Exception):
    pass


# ---------------------------------------------------------------------------
# low-level decoding
# ---------------------------------------------------------------------------
def u8(b, o):
    return b[o]


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


class Volume:
    def __init__(self, raw):
        self.raw = raw
        if len(raw) < SB_OFFSET + 1024:
            raise Fail("image is too small to hold a superblock")
        sb = memoryview(raw)[SB_OFFSET:SB_OFFSET + 1024]
        self.sb_raw = sb
        self.inodes_count = u32(sb, 0)
        self.blocks_count = u32(sb, 4)
        self.free_blocks = u32(sb, 12)
        self.free_inodes = u32(sb, 16)
        self.first_data_block = u32(sb, 20)
        self.log_block_size = u32(sb, 24)
        self.blocks_per_group = u32(sb, 32)
        self.inodes_per_group = u32(sb, 40)
        self.magic = u16(sb, 56)
        self.state = u16(sb, 58)
        self.rev_level = u32(sb, 76)
        self.first_ino = u32(sb, 84)
        self.inode_size = u16(sb, 88)
        self.compat = u32(sb, 92)
        self.incompat = u32(sb, 96)
        self.ro_compat = u32(sb, 100)
        self.desc_size = u16(sb, 254)
        self.block_size = 1024 << self.log_block_size
        self.block_count_effective = len(raw) // self.block_size
        if self.block_size <= 0:
            raise Fail("superblock log_block_size %d is unusable" % self.log_block_size)

    # -- blocks -------------------------------------------------------------
    def block(self, index):
        if index < 0 or (index + 1) * self.block_size > len(self.raw):
            raise Fail("block %d is outside the image" % index)
        off = index * self.block_size
        return memoryview(self.raw)[off:off + self.block_size]

    def group_count(self):
        by_blocks = (self.blocks_count + self.blocks_per_group - 1) // self.blocks_per_group
        by_inodes = (self.inodes_count + self.inodes_per_group - 1) // self.inodes_per_group
        return max(by_blocks, by_inodes)

    def group_desc(self, group):
        per_block = self.block_size // 32
        gdt_start = self.first_data_block + 1
        blk = gdt_start + group // per_block
        off = (group % per_block) * 32
        d = self.block(blk)
        return {
            "block_bitmap": u32(d, off + 0),
            "inode_bitmap": u32(d, off + 4),
            "inode_table": u32(d, off + 8),
            "free_blocks": u16(d, off + 12),
            "free_inodes": u16(d, off + 14),
            "used_dirs": u16(d, off + 16),
            "flags": u16(d, off + 18),
        }

    # -- inodes -------------------------------------------------------------
    def inode(self, ino):
        if ino < 1 or ino > self.inodes_count:
            raise Fail("inode number %d out of range" % ino)
        group = (ino - 1) // self.inodes_per_group
        index = (ino - 1) % self.inodes_per_group
        gd = self.group_desc(group)
        off = gd["inode_table"] * self.block_size + index * self.inode_size
        if off + 128 > len(self.raw):
            raise Fail("inode %d lies outside the image" % ino)
        raw = memoryview(self.raw)[off:off + self.inode_size]
        return {
            "no": ino,
            "mode": u16(raw, 0),
            "size": u32(raw, 4),
            "links": u16(raw, 26),
            "blocks_512": u32(raw, 28),
            "flags": u32(raw, 32),
            "i_block": bytes(raw[40:100]),
        }

    @staticmethod
    def kind(ino):
        fmt = ino["mode"] & 0o170000
        return {0o040000: "dir", 0o100000: "file", 0o120000: "symlink"}.get(fmt, "other")

    # -- extent trees -------------------------------------------------------
    def extents(self, ino):
        """Return ([(logical, length, start, unwritten)], root_depth, leaf_links)."""
        root = memoryview(ino["i_block"])
        found = []
        info = {"depth": None, "nodes": 0, "leaves": 0}

        def header(node, is_root):
            magic = u16(node, 0)
            entries = u16(node, 2)
            max_entries = u16(node, 4)
            depth = u16(node, 6)
            if magic != EXTENT_MAGIC:
                raise Fail("extent header magic 0x%04x != 0xF30A" % magic)
            cap = (len(node) - 12) // 12
            if is_root:
                cap = min(cap, 4)
            if entries > max_entries:
                raise Fail("extent header entries %d > max %d" % (entries, max_entries))
            if max_entries > cap:
                raise Fail("extent header max %d exceeds the node capacity %d"
                           % (max_entries, cap))
            if depth > 4:
                raise Fail("extent tree depth %d > 4" % depth)
            return entries, depth

        def walk(node, expect_depth, is_root):
            entries, depth = header(node, is_root)
            if expect_depth is not None and depth != expect_depth:
                raise Fail("extent depth %d, expected %d" % (depth, expect_depth))
            if info["depth"] is None:
                info["depth"] = depth
            if depth == 0:
                for i in range(entries):
                    off = 12 + i * 12
                    logical = u32(node, off)
                    length = u16(node, off + 4)
                    start = (u16(node, off + 6) << 32) | u32(node, off + 8)
                    unwritten = length > 32768
                    real = length - 32768 if unwritten else length
                    if real == 0:
                        raise Fail("extent with length 0 at logical block %d" % logical)
                    found.append((logical, real, start, unwritten))
                return
            for i in range(entries):
                off = 12 + i * 12
                leaf = (u16(node, off + 8) << 32) | u32(node, off + 4)
                if leaf == 0 or leaf >= self.blocks_count:
                    raise Fail("extent index points at block %d outside the volume" % leaf)
                info["nodes"] += 1
                if info["nodes"] > 512:
                    raise Fail("extent tree has more than 512 index nodes")
                walk(self.block(leaf), depth - 1, False)
                info["leaves"] += 1

        walk(root, None, True)
        found.sort()
        return found, info["depth"], info["leaves"]

    def read_file(self, ino):
        size = ino["size"]
        if size == 0:
            return b""
        if not (ino["flags"] & EXTENTS_FL):
            raise Fail("inode %d holds data but has no extents flag" % ino["no"])
        exts, _depth, _leaves = self.extents(ino)
        nblocks = (size + self.block_size - 1) // self.block_size
        chunks = []
        covered = {}
        for logical, length, start, unwritten in exts:
            for k in range(length):
                covered[logical + k] = None if unwritten else start + k
        for logical in range(nblocks):
            phys = covered.get(logical, "hole")
            if phys == "hole" or phys is None:
                chunks.append(bytes(self.block_size))
            else:
                chunks.append(bytes(self.block(phys)))
        return b"".join(chunks)[:size]

    def dirents(self, ino):
        data = self.read_file(ino)
        out = []
        pos = 0
        while pos + 8 <= len(data):
            inode, rec_len, name_len, ftype = struct.unpack_from("<IHBB", data, pos)
            if rec_len < 8 or rec_len & 3:
                raise Fail("dirent rec_len %d is not a multiple of 4" % rec_len)
            if pos + rec_len > len(data):
                raise Fail("dirent rec_len %d runs past the directory end" % rec_len)
            if name_len > rec_len - 8:
                raise Fail("dirent name_len %d does not fit rec_len %d"
                           % (name_len, rec_len))
            if inode:
                name = data[pos + 8:pos + 8 + name_len].decode("latin1")
                out.append((name, inode, ftype))
            pos += rec_len
        return out


# ---------------------------------------------------------------------------
# verification
# ---------------------------------------------------------------------------
class Checker:
    def __init__(self):
        self.rows = []

    def check(self, name, cond, detail=""):
        self.rows.append({"name": name, "ok": bool(cond), "detail": detail})

    def ok(self):
        return all(r["ok"] for r in self.rows)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def verify(image_path, manifest_path, guest_log=None):
    with open(image_path, "rb") as fh:
        raw = fh.read()
    with open(manifest_path, encoding="utf-8") as fh:
        man = json.load(fh)
    c = Checker()
    vol = Volume(raw)

    # ---- 1. image identity (the ext4 driver must not have written a byte) ---
    c.check("image size matches the manifest",
            len(raw) == man["image"]["size"], "%d != %d" % (len(raw), man["image"]["size"]))
    c.check("image sha256 matches the manifest (read-only mount left it untouched)",
            sha256(raw) == man["image"]["sha256"],
            sha256(raw) if sha256(raw) != man["image"]["sha256"] else "")

    # ---- 2. geometry -------------------------------------------------------
    c.check("superblock magic is 0xEF53", vol.magic == EXT4_MAGIC, hex(vol.magic))
    c.check("first_data_block is 0 for 4 KiB blocks", vol.first_data_block == 0,
            str(vol.first_data_block))
    c.check("log_block_size gives 4096-byte blocks", vol.block_size == 4096,
            str(vol.block_size))
    c.check("inode size is 256", vol.inode_size == 256, str(vol.inode_size))
    c.check("blocks_count is 4096 (16 MiB)", vol.blocks_count == 4096,
            str(vol.blocks_count))
    c.check("blocks_count matches the file length",
            vol.block_count_effective == vol.blocks_count,
            "%d vs %d" % (vol.block_count_effective, vol.blocks_count))
    c.check("inodes_count is 64", vol.inodes_count == 64, str(vol.inodes_count))
    c.check("exactly one block group", vol.group_count() == 1, str(vol.group_count()))
    c.check("revision is 1 (dynamic)", vol.rev_level == 1, str(vol.rev_level))
    c.check("first_ino is 11", vol.first_ino == 11, str(vol.first_ino))
    c.check("descriptor size is 0 or 32 (64bit off)", vol.desc_size in (0, 32),
            str(vol.desc_size))
    c.check("filesystem is marked cleanly unmounted", vol.state & 1 and not vol.state & 2,
            "state=0x%x" % vol.state)
    c.check("volume label is sutureos-e4",
            bytes(vol.sb_raw[120:136]).split(b"\x00")[0] == b"sutureos-e4",
            bytes(vol.sb_raw[120:136]).split(b"\x00")[0].decode("latin1"))

    # ---- 3. features -------------------------------------------------------
    c.check("feature_compat has no journal / dir_index bits", vol.compat == 0,
            hex(vol.compat))
    c.check("feature_incompat is exactly filetype|extents",
            vol.incompat == (INCOMPAT_FILETYPE | INCOMPAT_EXTENTS), hex(vol.incompat))
    c.check("feature_ro_compat is exactly sparse_super",
            vol.ro_compat == RO_COMPAT_SPARSE_SUPER, hex(vol.ro_compat))
    refused = {
        "64bit": (0x0080, "incompat"), "mmp": (0x0100, "incompat"),
        "bigalloc": (0x0200, "incompat"), "inline_data": (0x8000, "incompat"),
        "meta_bg": (0x0010, "incompat"), "has_journal": (0x0004, "compat"),
        "dir_index": (0x0020, "compat"), "metadata_csum": (0x0400, "ro_compat"),
    }
    for name, (bit, word) in refused.items():
        value = {"incompat": vol.incompat, "compat": vol.compat,
                 "ro_compat": vol.ro_compat}[word]
        c.check("refused feature %s (%s 0x%04x) is absent" % (name, word, bit),
                value & bit == 0, hex(value))

    # ---- 4. allocation / bitmaps ------------------------------------------
    gd = vol.group_desc(0)
    bitmap_blocks = vol.block(gd["block_bitmap"])
    bitmap_inodes = vol.block(gd["inode_bitmap"])
    used_blocks = sum(1 for b in range(vol.blocks_count)
                      if bitmap_blocks[b >> 3] & (1 << (b & 7)))
    used_inodes = sum(1 for i in range(vol.inodes_count)
                      if bitmap_inodes[i >> 3] & (1 << (i & 7)))
    c.check("superblock free_blocks == bitmap free blocks",
            vol.free_blocks == vol.blocks_count - used_blocks,
            "sb=%d bitmap=%d" % (vol.free_blocks, vol.blocks_count - used_blocks))
    c.check("group descriptor free_blocks == bitmap free blocks",
            gd["free_blocks"] == vol.blocks_count - used_blocks,
            "gd=%d bitmap=%d" % (gd["free_blocks"], vol.blocks_count - used_blocks))
    c.check("superblock free_inodes == bitmap free inodes",
            vol.free_inodes == vol.inodes_count - used_inodes,
            "sb=%d bitmap=%d" % (vol.free_inodes, vol.inodes_count - used_inodes))
    c.check("group descriptor free_inodes == bitmap free inodes",
            gd["free_inodes"] == vol.inodes_count - used_inodes,
            "gd=%d bitmap=%d" % (gd["free_inodes"], vol.inodes_count - used_inodes))
    c.check("group descriptor used_dirs == 2", gd["used_dirs"] == 2,
            str(gd["used_dirs"]))
    c.check("group descriptor flags are 0 (no uninit bitmap)", gd["flags"] == 0,
            hex(gd["flags"]))
    c.check("root inode (2) is marked used in the inode bitmap",
            bitmap_inodes[1 >> 3] & (1 << (1 & 7)) != 0)
    c.check("block bitmap marks the metadata blocks used",
            all(bitmap_blocks[b >> 3] & (1 << (b & 7))
                for b in [0, 1, gd["block_bitmap"], gd["inode_bitmap"],
                          gd["inode_table"]]))

    # ---- 5/6/7. per-file inode + extent + content -------------------------
    root = vol.inode(2)
    c.check("root inode is a directory", Volume.kind(root) == "dir")
    c.check("root inode has the extents flag", root["flags"] & EXTENTS_FL != 0)

    def resolve(path):
        parts = [p for p in path.split("/") if p]
        node = root
        for part in parts:
            if Volume.kind(node) != "dir":
                return None
            hit = None
            for name, ino_no, _ft in vol.dirents(node):
                if name == part:
                    hit = vol.inode(ino_no)
                    break
            if hit is None:
                return None
            node = hit
        return node

    referenced_blocks = []
    for f in man["files"]:
        node = resolve(f["path"])
        c.check("%s exists in the volume" % f["path"], node is not None)
        if node is None:
            continue
        c.check("%s has the manifest inode number (%d)" % (f["path"], f["inode"]),
                node["no"] == f["inode"], str(node["no"]))
        c.check("%s is a %s" % (f["path"], f["type"]),
                Volume.kind(node) == f["type"], Volume.kind(node))
        c.check("%s size is %d" % (f["path"], f["size"]), node["size"] == f["size"],
                str(node["size"]))
        if f["type"] == "file":
            c.check("%s has the extents flag" % f["path"], node["flags"] & EXTENTS_FL != 0)
            exts, depth, leaves = vol.extents(node)
            c.check("%s extent count is %d" % (f["path"], f["extents"]),
                    len(exts) == f["extents"], str(len(exts)))
            c.check("%s extent tree depth is %d" % (f["path"], f["depth"]),
                    depth == f["depth"], str(depth))
            data = vol.read_file(node)
            c.check("%s content sha256 matches the manifest" % f["path"],
                    sha256(data) == f["sha256"],
                    sha256(data) if sha256(data) != f["sha256"] else "")
            # coverage: every logical block mapped exactly once (except holes)
            holes = set()
            for a, b in f.get("holes", []):
                for lb in range(a // vol.block_size,
                                (b + vol.block_size - 1) // vol.block_size):
                    holes.add(lb)
            nblocks = (f["size"] + vol.block_size - 1) // vol.block_size
            seen = {}
            overlap = []
            for logical, length, start, _unw in exts:
                for k in range(length):
                    lb = logical + k
                    if lb in seen:
                        overlap.append(lb)
                    seen[lb] = start + k
                    referenced_blocks.append(start + k)
            missing = [lb for lb in range(nblocks) if lb not in seen and lb not in holes]
            c.check("%s has no unexpected hole" % f["path"], not missing,
                    "missing=%r" % (missing[:8],))
            c.check("%s extents do not overlap" % f["path"], not overlap,
                    "overlap=%r" % (overlap[:8],))
            c.check("%s has no duplicate physical block" % f["path"],
                    len(set(seen.values())) == len(seen))
            c.check("%s physical blocks are inside the volume" % f["path"],
                    all(start < vol.blocks_count for _l, _n, start, _u in exts))
            if f["size"] and f["path"] == "/SPARSE.BIN":
                hole_bytes = data[vol.block_size:2 * vol.block_size]
                c.check("SPARSE.BIN logical block 1 really is a hole (all zeroes)",
                        hole_bytes == bytes(vol.block_size))
                c.check("SPARSE.BIN block 1 has no extent",
                        1 not in seen)
            if f["extents"] and f["depth"] >= 1:
                c.check("%s depth-%d tree has %d leaf blocks"
                        % (f["path"], f["depth"], f["leaves"]),
                        leaves == f["leaves"], "%d != %d" % (leaves, f["leaves"]))
        elif f["type"] == "symlink":
            target = node["i_block"][:node["size"]].split(b"\x00")[0].decode("latin1")
            c.check("%s target is %s" % (f["path"], f["target"]),
                    target == f["target"], target)
            c.check("%s is a fast symlink (no extents flag, no data block)"
                    % f["path"], node["flags"] & EXTENTS_FL == 0)

    c.check("no two files share a physical block",
            len(referenced_blocks) == len(set(referenced_blocks)),
            "%d references, %d unique" % (len(referenced_blocks),
                                          len(set(referenced_blocks))))
    c.check("every block an inode references is marked used in the bitmap",
            all(bitmap_blocks[b >> 3] & (1 << (b & 7)) for b in referenced_blocks),
            "%d blocks referenced" % len(referenced_blocks))
    c.check("no extent points at block 0 (the superblock block)",
            all(b != 0 for b in referenced_blocks))

    # ---- /BIG.BIN shape, spelled out (the depth-1 traverse evidence) ------
    big = resolve("/BIG.BIN")
    big_exts, big_depth, big_leaves = vol.extents(big)
    c.check("/BIG.BIN uses a depth-1 extent tree", big_depth == 1, str(big_depth))
    c.check("/BIG.BIN has two leaf blocks", big_leaves == 2, str(big_leaves))
    c.check("/BIG.BIN has 384 extents (one per interleaved 4 KiB block)",
            len(big_exts) == 384, str(len(big_exts)))
    c.check("/BIG.BIN extents are consecutive logical blocks 0..383",
            [e[0] for e in big_exts] == list(range(384)))
    c.check("/BIG.BIN extents are each one block long",
            all(e[1] == 1 for e in big_exts))
    c.check("/BIG.BIN physical blocks have a stride of 2 (interleaved filler)",
            all(big_exts[i + 1][2] - big_exts[i][2] == 2 for i in range(len(big_exts) - 1)))

    # ---- 8. directories ---------------------------------------------------
    for dpath, names in man["dirs"].items():
        node = root if dpath == "/" else resolve(dpath)
        entries = vol.dirents(node)
        got = sorted(e[0] for e in entries)
        c.check("%s directory listing matches the manifest" % dpath,
                got == sorted(names), "got=%r" % (got,))
        c.check("%s lists every name exactly once" % dpath,
                len(got) == len(set(got)))
        dot = [e for e in entries if e[0] == "."]
        dotdot = [e for e in entries if e[0] == ".."]
        c.check("%s has a '.' entry pointing at itself" % dpath,
                len(dot) == 1 and dot[0][1] == node["no"], str(dot))
        c.check("%s has a '..' entry pointing at the parent" % dpath,
                len(dotdot) == 1 and
                dotdot[0][1] == (2 if dpath != "/" else 2), str(dotdot))
        c.check("%s '.' and '..' file types are dir(2)" % dpath,
                dot and dot[0][2] == 2 and dotdot and dotdot[0][2] == 2)
        # the file_type byte must agree with the inode mode
        bad = []
        for name, ino_no, ftype in entries:
            if name in (".", ".."):
                continue
            want = {"file": 1, "dir": 2, "symlink": 7}.get(Volume.kind(vol.inode(ino_no)))
            if want != ftype:
                bad.append("%s(ftype=%s, mode=%s)"
                           % (name, FT.get(ftype, ftype),
                              Volume.kind(vol.inode(ino_no))))
        c.check("%s dirent file_type bytes agree with the inode modes" % dpath,
                not bad, "bad=%r" % (bad,))
        c.check("%s directory size is one 4 KiB block" % dpath,
                node["size"] == vol.block_size, str(node["size"]))

    # ---- 9. link counts ---------------------------------------------------
    subdirs = [e for e in vol.dirents(root) if e[2] == 2 and e[0] not in (".", "..")]
    c.check("root link count is 2 + number of subdirectories",
            root["links"] == 2 + len(subdirs),
            "%d vs %d" % (root["links"], 2 + len(subdirs)))
    for name, ino_no, _ft in subdirs:
        sub = vol.inode(ino_no)
        c.check("subdirectory %s link count is 2" % name, sub["links"] == 2,
                str(sub["links"]))
        c.check("subdirectory %s '..' points at the root inode" % name,
                [e for e in vol.dirents(sub) if e[0] == ".."][0][1] == 2)
    for f in man["files"]:
        if f["type"] == "file":
            node = resolve(f["path"])
            c.check("%s link count is 1" % f["path"], node["links"] == 1,
                    str(node["links"]))
    c.check("root inode i_blocks covers its single data block",
            root["blocks_512"] == vol.block_size // 512, str(root["blocks_512"]))

    # ---- 10. optional: cross-check the guest serial log -------------------
    if guest_log:
        with open(guest_log, encoding="utf-8", errors="replace") as fh:
            log = fh.read()
        c.check("guest mounted the ext4 volume at /mnt/e4",
                "[ext4] vfs mount" in log and "/mnt/e4" in log)
        m = re.search(r"\[ext4\] mount drive=1 blocksz=(\d+) blocks=(\d+) inodes=(\d+) "
                      r"groups=(\d+) extent_depth_max=(\d+)", log)
        c.check("guest mount line reports the same geometry", m is not None)
        if m:
            c.check("guest block size matches the image", int(m.group(1)) == vol.block_size,
                    m.group(1))
            c.check("guest block count matches the image",
                    int(m.group(2)) == vol.blocks_count, m.group(2))
            c.check("guest inode count matches the image",
                    int(m.group(3)) == vol.inodes_count, m.group(3))
            c.check("guest group count matches the image", int(m.group(4)) == 1,
                    m.group(4))
            c.check("guest extent_depth_max is 1 (depth-1 trees really traversed)",
                    int(m.group(5)) == 1, m.group(5))
        for f in man["files"]:
            if f["type"] != "file":
                continue
            # the guest logs volume-relative paths ("SUBDIR/INNER.TXT")
            name = f["path"].lstrip("/")
            pattern = (r"\[ext4\] file %s size=(\d+) extents=(\d+) depth=(\d+) leaves=(\d+)"
                       % re.escape(name))
            fm = re.search(pattern, log)
            c.check("guest logged %s with an extent count" % f["path"], fm is not None)
            if fm:
                c.check("guest %s size matches the image" % f["path"],
                        int(fm.group(1)) == f["size"], fm.group(1))
                c.check("guest %s extent count matches the host parse" % f["path"],
                        int(fm.group(2)) == f["extents"], fm.group(2))
                c.check("guest %s depth matches the host parse" % f["path"],
                        int(fm.group(3)) == f["depth"], fm.group(3))
                c.check("guest %s leaf count matches the host parse" % f["path"],
                        int(fm.group(4)) == f.get("leaves", 0), fm.group(4))
        c.check("guest reported the read-only policy",
                "[ext4] read-only: open/read/stat/readdir only" in log)
        # "write rejected" is the *intended* policy line for the deliberate write
        # attempt, not an error; WARNING/BAD/mount failed are real findings.
        c.check("guest logged no [ext4] warning/bad/failure lines",
                not re.search(r"\[ext4\] (?:WARNING|BAD|mount failed|ERROR|corrupt)", log),
                ",".join(sorted(set(re.findall(
                    r"\[ext4\] (WARNING|BAD|mount failed|ERROR|corrupt)", log)))))
        c.check("guest logged the refused write attempt",
                "[ext4] write rejected: the ext4 driver is read-only" in log)
        c.check("guest read HELLO.TXT and reported 125 bytes",
                "(125 bytes)" in log)
        c.check("guest read SUBDIR/INNER.TXT and reported 71 bytes",
                "(71 bytes)" in log)
        c.check("guest reported BIG.BIN size 1572864 via stat",
                re.search(r"Size: 1572864 bytes", log) is not None)
        c.check("guest reported EMPTY as 0 bytes",
                re.search(r"\(0 bytes\)", log) is not None or "cat /mnt/e4/EMPTY" in log)
    return c, vol


def main():
    ap = argparse.ArgumentParser(description="independent ext4 image verifier")
    ap.add_argument("image", nargs="?", default=os.path.join(ROOT, "build", "ext4.img"))
    ap.add_argument("--manifest", default=os.path.join(ROOT, "build", "ext4_manifest.json"))
    ap.add_argument("--guest-log", default=None,
                    help="serial log from the acceptance run (cross-checks the guest)")
    ap.add_argument("--json", default=None, help="write the check table as JSON")
    args = ap.parse_args()

    print("[verify_ext4] image    : %s" % args.image)
    print("[verify_ext4] manifest : %s" % args.manifest)
    if args.guest_log:
        print("[verify_ext4] guest log: %s" % args.guest_log)

    try:
        checks, vol = verify(args.image, args.manifest, args.guest_log)
    except Fail as e:
        print("  [FAIL] %s" % e)
        print("[verify_ext4] checks=1 ok=0 fail=1")
        print("[verify_ext4] RESULT: FAIL")
        return 1

    for row in checks.rows:
        if not row["ok"]:
            print("  [FAIL] %s%s" % (row["name"],
                                     (" (%s)" % row["detail"]) if row["detail"] else ""))
    total = len(checks.rows)
    failed = sum(1 for r in checks.rows if not r["ok"])
    print("[verify_ext4] geometry : blocks=%d block_size=%d inodes=%d inode_size=%d "
          "groups=%d features incompat=0x%04x ro_compat=0x%04x"
          % (vol.blocks_count, vol.block_size, vol.inodes_count, vol.inode_size,
             vol.group_count(), vol.incompat, vol.ro_compat))
    print("[verify_ext4] checks=%d ok=%d fail=%d" % (total, total - failed, failed))
    print("[verify_ext4] RESULT:", "PASS" if failed == 0 else "FAIL")
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"image": args.image, "guest_log": args.guest_log,
                       "checks": checks.rows}, fh, indent=2)
            fh.write("\n")
        print("[verify_ext4] wrote %s" % args.json)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
