#!/usr/bin/env python3
"""Build a REAL ext2 (revision 1) test image for SutureOS - pure Python stdlib.

Default output (both files, same contents, different block size):

  build/ext2.img     4 MiB, 1024-byte blocks, 1 block group  (primary)
  build/ext2_4k.img  4 MiB, 4096-byte blocks, 1 block group  (block-size coverage)

Filesystem contents (identical in both images; sizes are in bytes):

  /HELLO.TXT       short text file, single direct block
  /BIG.BIN         64 KiB deterministic pattern; needs i_block[12]
                   (the single-indirect block) with 1 KiB blocks AND with
                   4 KiB blocks (>48 KiB is past the 12 direct blocks)
  /SPARSE.BIN      3 blocks; logical block 1 is a HOLE (i_block[] == 0).
                   The bytes of that logical block must read back as zeros.
  /DOUBLE.BIN      300 KiB deterministic pattern; at 1 KiB blocks this needs
                   i_block[13] (the double-indirect tree): 12 direct + 256
                   single-indirect + 32 double-indirect data blocks.
  /SUB/            subdirectory holding NESTED.TXT (exercises nested dir
                   traversal and the ".." entry)

CONTENT RULES - byte i (0-based) of each file.  These MUST stay in sync with
kernel/fs/ext2.c (ext2_selftest() re-derives them in the guest):

  HELLO.TXT   : the literal ASCII text in HELLO_TXT below
  NESTED.TXT  : the literal ASCII text in NESTED_TXT below
  BIG.BIN     : b[i] = (i * 31 + 7) & 0xFF
  SPARSE.BIN  : b[i] = (i * 17 + 3) & 0xFF, except every byte of logical
                block 1 (the hole), which is 0
  DOUBLE.BIN  : b[i] = (i * 29 + 11) & 0xFF

The script re-parses its own output with an independent parser (fixed on-disk
offsets, block-pointer walking, bitmap cross-checks) and prints
`host-side verification: PASS`.  No external tools (no mke2fs) are used.

Usage:
  py -3 scripts/mkfs_ext2_testimg.py
  py -3 scripts/mkfs_ext2_testimg.py --blocksize 1024 --out build/ext2.img
"""

import argparse
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "build")

IMG_SIZE = 4 * 1024 * 1024        # 4 MiB
SECTOR = 512
INODE_SIZE = 128
INODES_PER_GROUP = 64
BLOCKS_PER_GROUP = 8192           # one group covers >= the whole 4 MiB image
FIRST_INO = 11                    # first non-reserved inode (ext2 convention)
MTIME = 1700000000                # deterministic timestamp
EXT2_MAGIC = 0xEF53
EXT2_VALID_FS = 0x0001
EXT2_ERRORS_CONTINUE = 1
EXT2_FEATURE_INCOMPAT_FILETYPE = 0x0002

# Directory entry file types (ext2 ft_* values)
FT_UNKNOWN, FT_REG, FT_DIR = 0, 1, 2

# inode numbers used by this image
INO_ROOT = 2
INO_HELLO = 11
INO_BIG = 12
INO_SUB = 13
INO_NESTED = 14
INO_SPARSE = 15
INO_DOUBLE = 16
RESERVED_INODES = 10              # inodes 1..10 are reserved in ext2

HELLO_TXT = b"Hello from SutureOS ext2!\nSecond line: read via the real ext2 driver.\n"
NESTED_TXT = b"Nested file: SUB/NESTED.TXT reached through ext2 directory traversal.\n"

BIG_BIN_SIZE = 64 * 1024          # 64 KiB -> single indirect at 1K and 4K
DOUBLE_BIN_SIZE = 300 * 1024      # 300 KiB -> double indirect at 1K blocks
SPARSE_BIN_BLOCKS = 3             # logical block 1 is the hole
SPARSE_HOLE_BLOCK = 1


def big_byte(i):
    return (i * 31 + 7) & 0xFF


def double_byte(i):
    return (i * 29 + 11) & 0xFF


def sparse_byte(i, block_size):
    if (i // block_size) == SPARSE_HOLE_BLOCK:
        return 0
    return (i * 17 + 3) & 0xFF


def make_big():
    return bytes(big_byte(i) for i in range(BIG_BIN_SIZE))


def make_double():
    return bytes(double_byte(i) for i in range(DOUBLE_BIN_SIZE))


def make_sparse(block_size):
    size = SPARSE_BIN_BLOCKS * block_size
    return bytes(sparse_byte(i, block_size) for i in range(size))


def dirent(ino, name, ftype, rec_len):
    """One ext2 directory entry padded to rec_len bytes."""
    need = (8 + len(name) + 3) & ~3
    if rec_len < need:
        raise AssertionError("rec_len %d < minimum %d for %r" % (rec_len, need, name))
    buf = bytearray(rec_len)
    struct.pack_into("<IHBB", buf, 0, ino, rec_len, len(name), ftype)
    buf[8:8 + len(name)] = name
    return bytes(buf)


def build_dir_block(entries, block_size):
    """Serialize (ino, name, ftype) entries into exactly one data block."""
    out = bytearray()
    for i, (ino, name, ftype) in enumerate(entries):
        if i == len(entries) - 1:
            rec = block_size - len(out)      # last entry swallows the slack
        else:
            rec = (8 + len(name) + 3) & ~3
        out += dirent(ino, name, ftype, rec)
    assert len(out) == block_size, (len(out), block_size)
    return bytes(out)


class ImageBuilder(object):
    """Deterministic ext2 rev1 image writer (single block group)."""

    def __init__(self, block_size, total_blocks):
        self.bs = block_size
        self.total_blocks = total_blocks
        self.img = bytearray(total_blocks * block_size)
        self.used = set()
        self.next_free = 0

        # ---- fixed metadata layout ----
        # The superblock always lives at byte offset 1024: inside block 1 for
        # 1 KiB blocks and inside block 0 for larger blocks.
        self.first_data_block = 1 if block_size == 1024 else 0
        self.gdt_block = self.first_data_block + 1
        self.block_bitmap_block = self.gdt_block + 1
        self.inode_bitmap_block = self.block_bitmap_block + 1
        self.inode_table_block = self.inode_bitmap_block + 1
        self.inode_table_blocks = ((INODES_PER_GROUP * INODE_SIZE) + block_size - 1) // block_size
        self.alloc_start = self.inode_table_block + self.inode_table_blocks

        # mark boot block 0..first_data_block-1 and all metadata as used
        for b in range(0, self.alloc_start):
            self.used.add(b)
        self.next_free = self.alloc_start
        self.inodes = {}                 # ino -> dict(mode,size,links,blocks,ptrs)
        self.dir_blocks = {}             # ino -> data block number

    # ---------------- low level ----------------
    def block_off(self, blk):
        return blk * self.bs

    def write_block(self, blk, data):
        if len(data) > self.bs:
            raise AssertionError("block %d: %d bytes > block size" % (blk, len(data)))
        off = self.block_off(blk)
        self.img[off:off + len(data)] = data

    def read_block(self, blk):
        off = self.block_off(blk)
        return bytes(self.img[off:off + self.bs])

    def alloc(self, count=1):
        """Allocate `count` consecutive blocks (first fit, forward only)."""
        blk = self.next_free
        while True:
            clash = any((blk + i) in self.used for i in range(count))
            if not clash and blk + count <= self.total_blocks:
                break
            blk += 1
            if blk + count > self.total_blocks:
                raise AssertionError("out of blocks")
        for i in range(count):
            self.used.add(blk + i)
        self.next_free = blk + count
        return blk

    def put_pointer_table(self, blk, values):
        buf = bytearray(self.bs)
        for i, v in enumerate(values):
            struct.pack_into("<I", buf, i * 4, v)
        self.write_block(blk, bytes(buf))

    # ---------------- file placement ----------------
    def place_file(self, ino, content, hole_blocks=()):
        """Allocate blocks for `content` and record the inode block pointers.

        Returns a dict describing how the file is mapped (for diagnostics).
        """
        bs = self.bs
        ptrs = bs // 4
        nblocks = (len(content) + bs - 1) // bs
        logical = [0] * nblocks
        for lb in range(nblocks):
            if lb in hole_blocks:
                logical[lb] = 0            # hole: pointer stays 0
                continue
            blk = self.alloc(1)
            chunk = content[lb * bs:(lb + 1) * bs]
            self.write_block(blk, chunk)
            logical[lb] = blk

        i_block = [0] * 15
        meta_blocks = 0
        for lb in range(min(nblocks, 12)):
            i_block[lb] = logical[lb]
        if nblocks > 12:
            si = self.alloc(1)
            meta_blocks += 1
            i_block[12] = si
            table = [0] * ptrs
            for idx, lb in enumerate(range(12, min(nblocks, 12 + ptrs))):
                table[idx] = logical[lb]
            self.put_pointer_table(si, table)
        if nblocks > 12 + ptrs:
            di = self.alloc(1)
            meta_blocks += 1
            i_block[13] = di
            dtable = [0] * ptrs
            children = {}
            for lb in range(12 + ptrs, nblocks):
                off = lb - 12 - ptrs
                idx1, idx2 = divmod(off, ptrs)
                if idx1 not in children:
                    children[idx1] = [0] * ptrs
                    dtable[idx1] = self.alloc(1)
                    meta_blocks += 1
                children[idx1][idx2] = logical[lb]
            self.put_pointer_table(di, dtable)
            for idx1, table in children.items():
                self.put_pointer_table(dtable[idx1], table)
        if nblocks > 12 + ptrs + ptrs * ptrs:
            raise AssertionError("triple indirect is out of scope for this image")

        self.inodes[ino] = {
            "mode": 0o100644,
            "size": len(content),
            "links": 1,
            "ptrs": i_block,
            "blocks": (nblocks - len(hole_blocks) + meta_blocks) * (bs // SECTOR),
        }
        return {"nblocks": nblocks, "logical": logical, "meta": meta_blocks}

    def add_dir(self, ino, entries, links):
        """Create a directory: one data block with `.`/`..` plus `entries`."""
        blk = self.alloc(1)
        self.dir_blocks[ino] = blk
        rows = [(ino, b".", FT_DIR), (links["parent"], b"..", FT_DIR)] + list(entries)
        self.write_block(blk, build_dir_block(rows, self.bs))
        self.inodes[ino] = {
            "mode": 0o040755,
            "size": self.bs,
            "links": links["self"],
            "ptrs": [blk] + [0] * 14,
            "blocks": self.bs // SECTOR,
        }
        return blk

    # ---------------- metadata ----------------
    def write_inode_table(self):
        for ino, info in self.inodes.items():
            off = self.block_off(self.inode_table_block) + (ino - 1) * INODE_SIZE
            inode = bytearray(INODE_SIZE)
            struct.pack_into("<HHIIIIIHHIII", inode, 0,
                             info["mode"], 0,                  # i_mode, i_uid
                             info["size"],                     # i_size
                             MTIME, MTIME, MTIME, 0,           # atime, ctime, mtime, dtime
                             0, info["links"],                 # i_gid, i_links_count
                             info["blocks"], 0, 0)             # i_blocks, i_flags, i_osd1
            for i in range(15):
                struct.pack_into("<I", inode, 40 + i * 4, info["ptrs"][i])
            self.img[off:off + INODE_SIZE] = inode
        # inode 1 (bad blocks) is reserved: all zeroes, which the bytearray
        # already holds.

    def write_bitmaps(self):
        bs = self.bs
        # block bitmap: real used blocks + padding bits beyond total_blocks
        bm = bytearray(bs)
        group_blocks = BLOCKS_PER_GROUP
        for b in range(group_blocks):
            used = (b < self.total_blocks and b in self.used) or (b >= self.total_blocks)
            if used:
                bm[b // 8] |= 1 << (b % 8)
        self.write_block(self.block_bitmap_block, bytes(bm))

        # inode bitmap: inodes 1..10 reserved + every inode we created
        im = bytearray(bs)
        for ino in range(1, INODES_PER_GROUP + 1):
            used = ino <= RESERVED_INODES or ino in self.inodes
            if used:
                im[(ino - 1) // 8] |= 1 << ((ino - 1) % 8)
        self.write_block(self.inode_bitmap_block, bytes(im))
    def free_blocks(self):
        return self.total_blocks - len(self.used)

    def used_inode_count(self):
        """Reserved inodes 1..10 plus every inode this builder created
        (the root inode 2 overlaps the reserved range and counts once)."""
        used = set(range(1, RESERVED_INODES + 1))
        used |= set(self.inodes.keys())
        return len(used)

    def free_inodes(self):
        return INODES_PER_GROUP - self.used_inode_count()

    def write_superblock(self):
        sb = bytearray(1024)

        def u8(off, v):
            sb[off] = v & 0xFF

        def u16(off, v):
            struct.pack_into("<H", sb, off, v & 0xFFFF)

        def u32(off, v):
            struct.pack_into("<I", sb, off, v & 0xFFFFFFFF)

        dirs = sum(1 for i in self.inodes.values() if (i["mode"] & 0o170000) == 0o040000)
        u32(0, INODES_PER_GROUP)
        u32(4, self.total_blocks)
        u32(8, 0)                                  # r_blocks_count
        u32(12, self.free_blocks())
        u32(16, self.free_inodes())
        u32(20, self.first_data_block)
        u32(24, {1024: 0, 2048: 1, 4096: 2}[self.bs])   # s_log_block_size
        u32(28, {1024: 0, 2048: 1, 4096: 2}[self.bs])   # s_log_frag_size
        u32(32, BLOCKS_PER_GROUP)
        u32(36, BLOCKS_PER_GROUP)
        u32(40, INODES_PER_GROUP)
        u32(44, MTIME)                             # s_mtime
        u32(48, MTIME)                             # s_wtime
        u16(52, 0)                                 # s_mnt_count
        u16(54, 0xFFFF)                            # s_max_mnt_count (-1)
        u16(56, EXT2_MAGIC)
        u16(58, EXT2_VALID_FS)                     # clean
        u16(60, EXT2_ERRORS_CONTINUE)
        u16(62, 0)                                 # s_minor_rev_level
        u32(64, MTIME)                             # s_lastcheck
        u32(68, 0xFFFFFFFF)                        # s_checkinterval
        u32(72, 0)                                 # s_creator_os = Linux
        u32(76, 1)                                 # s_rev_level = 1
        u16(80, 0)                                 # def_resuid
        u16(82, 0)                                 # def_resgid
        u32(84, FIRST_INO)
        u16(88, INODE_SIZE)
        u16(90, 0)                                 # s_block_group_nr
        u32(92, 0)                                 # feature_compat
        u32(96, EXT2_FEATURE_INCOMPAT_FILETYPE)
        u32(100, 0)                                # feature_ro_compat
        sb[104:120] = bytes(range(0x10, 0x20))     # deterministic UUID
        name = b"sutureos-ext2"
        sb[120:120 + len(name)] = name
        mnt = b"/mnt/ext2"
        sb[136:136 + len(mnt)] = mnt
        u32(200, 0)                                # algorithm_usage_bitmap
        u8(204, 0)                                 # prealloc_blocks
        u8(205, 0)                                 # prealloc_dir_blocks
        u16(206, 0)                                # reserved_gdt_blocks
        u32(224, 0)                                # journal_inum
        u32(228, 0)                                # journal_dev
        u32(232, 0)                                # last_orphan
        u8(252, 0)                                 # def_hash_version
        u8(253, 0)                                 # jnl_backup_type
        u16(254, 0)                                # desc_size (0 => 32 bytes)
        u32(256, 0)                                # default_mount_opts
        u32(260, 0)                                # first_meta_bg
        u32(264, MTIME)                            # mkfs_time

        # superblock lives at byte offset 1024 inside block sb_block
        self.img[1024:2048] = bytes(sb)

    def write_gdt(self):
        gd = bytearray(32)
        struct.pack_into("<IIIHHHH", gd, 0,
                         self.block_bitmap_block, self.inode_bitmap_block,
                         self.inode_table_block,
                         self.free_blocks(), self.free_inodes(), 2, 0)
        self.write_block(self.gdt_block, bytes(gd))


def build_image(block_size):
    """Create the image and return (bytes, info-dict)."""
    total_blocks = IMG_SIZE // block_size
    b = ImageBuilder(block_size, total_blocks)

    big = make_big()
    sparse = make_sparse(block_size)
    dbl = make_double()
    hello = HELLO_TXT
    nested = NESTED_TXT

    # allocation order is fixed and deterministic
    root_blk = b.alloc(1)
    sub_blk = b.alloc(1)
    b.dir_blocks[INO_ROOT] = root_blk
    b.dir_blocks[INO_SUB] = sub_blk

    place_kind = {}
    place_kind[INO_HELLO] = b.place_file(INO_HELLO, hello)
    place_kind[INO_NESTED] = b.place_file(INO_NESTED, nested)
    place_kind[INO_BIG] = b.place_file(INO_BIG, big)
    place_kind[INO_SPARSE] = b.place_file(INO_SPARSE, sparse,
                                          hole_blocks=(SPARSE_HOLE_BLOCK,))
    place_kind[INO_DOUBLE] = b.place_file(INO_DOUBLE, dbl)

    # directories (written after the file blocks so `.`/`..` inodes exist)
    b.write_block(root_blk, build_dir_block([
        (INO_ROOT, b".", FT_DIR),
        (INO_ROOT, b"..", FT_DIR),
        (INO_HELLO, b"HELLO.TXT", FT_REG),
        (INO_BIG, b"BIG.BIN", FT_REG),
        (INO_SPARSE, b"SPARSE.BIN", FT_REG),
        (INO_DOUBLE, b"DOUBLE.BIN", FT_REG),
        (INO_SUB, b"SUB", FT_DIR),
    ], block_size))
    b.write_block(sub_blk, build_dir_block([
        (INO_SUB, b".", FT_DIR),
        (INO_ROOT, b"..", FT_DIR),
        (INO_NESTED, b"NESTED.TXT", FT_REG),
    ], block_size))

    # root: 2 + one subdirectory; SUB: 2
    b.inodes[INO_ROOT] = {"mode": 0o040755, "size": block_size, "links": 3,
                          "ptrs": [root_blk] + [0] * 14, "blocks": block_size // SECTOR}
    b.inodes[INO_SUB] = {"mode": 0o040755, "size": block_size, "links": 2,
                         "ptrs": [sub_blk] + [0] * 14, "blocks": block_size // SECTOR}

    b.write_inode_table()
    b.write_bitmaps()
    b.write_superblock()
    b.write_gdt()

    info = {
        "block_size": block_size,
        "total_blocks": total_blocks,
        "used_blocks": len(b.used),
        "free_blocks": b.free_blocks(),
        "free_inodes": b.free_inodes(),
        "place": place_kind,
        "expect": {
            "/HELLO.TXT": hello,
            "/BIG.BIN": big,
            "/SUB/NESTED.TXT": nested,
            "/SPARSE.BIN": sparse,
            "/DOUBLE.BIN": dbl,
        },
        "inodes": {
            "/HELLO.TXT": INO_HELLO,
            "/BIG.BIN": INO_BIG,
            "/SUB": INO_SUB,
            "/SUB/NESTED.TXT": INO_NESTED,
            "/SPARSE.BIN": INO_SPARSE,
            "/DOUBLE.BIN": INO_DOUBLE,
        },
    }
    return bytes(b.img), info


# ==========================================================================
# Independent host-side parser: reads the produced bytes back with fixed
# on-disk offsets (it shares no state with ImageBuilder) and checks the
# superblock, the bitmaps, the directory tree, the block pointers and the
# byte content of every file.
# ==========================================================================

def parse_superblock(img):
    if len(img) < 2048:
        raise AssertionError("image too small to hold a superblock")
    sb = img[1024:2048]
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
    f["errors"] = struct.unpack_from("<H", sb, 60)[0]
    f["rev_level"] = struct.unpack_from("<I", sb, 76)[0]
    f["first_ino"] = struct.unpack_from("<I", sb, 84)[0]
    f["inode_size"] = struct.unpack_from("<H", sb, 88)[0]
    f["feature_compat"] = struct.unpack_from("<I", sb, 92)[0]
    f["feature_incompat"] = struct.unpack_from("<I", sb, 96)[0]
    f["feature_ro_compat"] = struct.unpack_from("<I", sb, 100)[0]
    f["block_size"] = 1024 << f["log_block_size"]
    return f


def parse_group_desc(img, sb, group):
    bs = sb["block_size"]
    gdt_block = sb["first_data_block"] + 1
    off = gdt_block * bs + group * 32
    g = {}
    g["block_bitmap"] = struct.unpack_from("<I", img, off)[0]
    g["inode_bitmap"] = struct.unpack_from("<I", img, off + 4)[0]
    g["inode_table"] = struct.unpack_from("<I", img, off + 8)[0]
    g["free_blocks_count"] = struct.unpack_from("<H", img, off + 12)[0]
    g["free_inodes_count"] = struct.unpack_from("<H", img, off + 14)[0]
    g["used_dirs_count"] = struct.unpack_from("<H", img, off + 16)[0]
    return g


def parse_inode(img, sb, g, ino):
    bs = sb["block_size"]
    isize = sb["inode_size"]
    if isize <= 0 or isize > bs:
        raise AssertionError("inode size %d incompatible with block size %d" % (isize, bs))
    off = g["inode_table"] * bs + ((ino - 1) % sb["inodes_per_group"]) * isize
    raw = img[off:off + isize]
    if len(raw) != isize:
        raise AssertionError("inode %d outside image" % ino)
    i = {}
    i["mode"] = struct.unpack_from("<H", raw, 0)[0]
    i["size"] = struct.unpack_from("<I", raw, 4)[0]
    i["mtime"] = struct.unpack_from("<I", raw, 16)[0]
    i["links"] = struct.unpack_from("<H", raw, 26)[0]
    i["blocks"] = struct.unpack_from("<I", raw, 28)[0]
    i["block"] = [struct.unpack_from("<I", raw, 40 + k * 4)[0] for k in range(15)]
    return i


def resolve_block(img, sb, inode, logical):
    """Walk direct / single / double / triple indirect pointers.
    0 means hole (or an unmapped level)."""
    bs = sb["block_size"]
    ptrs = bs // 4
    blk = inode["block"]
    if logical < 12:
        return blk[logical]
    if logical < 12 + ptrs:
        if blk[12] == 0:
            return 0
        table = img[blk[12] * bs:(blk[12] + 1) * bs]
        return struct.unpack_from("<I", table, (logical - 12) * 4)[0]
    if logical < 12 + ptrs + ptrs * ptrs:
        if blk[13] == 0:
            return 0
        d = img[blk[13] * bs:(blk[13] + 1) * bs]
        off = logical - 12 - ptrs
        idx1, idx2 = divmod(off, ptrs)
        child = struct.unpack_from("<I", d, idx1 * 4)[0]
        if child == 0:
            return 0
        c = img[child * bs:(child + 1) * bs]
        return struct.unpack_from("<I", c, idx2 * 4)[0]
    if logical < 12 + ptrs + ptrs * ptrs + ptrs * ptrs * ptrs:
        if blk[14] == 0:
            return 0
        l1 = img[blk[14] * bs:(blk[14] + 1) * bs]
        off = logical - 12 - ptrs - ptrs * ptrs
        i1, rest = divmod(off, ptrs * ptrs)
        i2, i3 = divmod(rest, ptrs)
        b1 = struct.unpack_from("<I", l1, i1 * 4)[0]
        if b1 == 0:
            return 0
        l2 = img[b1 * bs:(b1 + 1) * bs]
        b2 = struct.unpack_from("<I", l2, i2 * 4)[0]
        if b2 == 0:
            return 0
        l3 = img[b2 * bs:(b2 + 1) * bs]
        return struct.unpack_from("<I", l3, i3 * 4)[0]
    return 0


def read_file_bytes(img, sb, inode):
    bs = sb["block_size"]
    size = inode["size"]
    out = bytearray()
    for logical in range((size + bs - 1) // bs):
        phys = resolve_block(img, sb, inode, logical)
        if phys == 0:
            out += b"\x00" * bs
        else:
            out += img[phys * bs:(phys + 1) * bs]
    return bytes(out[:size])


def walk_dir(img, sb, inode):
    """Return [(ino, name, file_type)] for a directory inode."""
    bs = sb["block_size"]
    data = read_file_bytes(img, sb, inode)
    entries = []
    pos = 0
    while pos + 8 <= len(data):
        ino, rec_len, name_len, ftype = struct.unpack_from("<IHBB", data, pos)
        if rec_len == 0 or rec_len % 4 or pos + rec_len > len(data):
            raise AssertionError("bad rec_len %d at %d" % (rec_len, pos))
        if name_len > rec_len - 8:
            raise AssertionError("bad name_len %d at %d" % (name_len, pos))
        name = data[pos + 8:pos + 8 + name_len]
        if ino != 0:
            entries.append((ino, name, ftype))
        pos += rec_len
    if pos != len(data):
        raise AssertionError("directory entries do not fill the directory")
    return entries


def find_entry(entries, name):
    for ino, nm, ft in entries:
        if nm == name:
            return ino, ft
    return None, None


def verify_image(img, info):
    """Independent re-parse. Raises AssertionError on any inconsistency."""
    sb = parse_superblock(img)
    assert sb["magic"] == EXT2_MAGIC, "magic 0x%x" % sb["magic"]
    assert sb["rev_level"] == 1, "rev_level %d" % sb["rev_level"]
    assert sb["block_size"] == info["block_size"], "block size mismatch"
    assert sb["blocks_count"] * sb["block_size"] == len(img), \
        "blocks_count %d does not match image size" % sb["blocks_count"]
    assert sb["inode_size"] == INODE_SIZE, "inode_size %d" % sb["inode_size"]
    assert sb["inodes_per_group"] == INODES_PER_GROUP
    assert sb["state"] & EXT2_VALID_FS, "filesystem not clean (state=0x%x)" % sb["state"]
    assert sb["errors"] == EXT2_ERRORS_CONTINUE
    groups = (sb["blocks_count"] + sb["blocks_per_group"] - 1) // sb["blocks_per_group"]
    assert groups == 1, "expected 1 group, got %d" % groups
    assert sb["inodes_count"] == INODES_PER_GROUP
    assert sb["feature_incompat"] & EXT2_FEATURE_INCOMPAT_FILETYPE, "filetype feature missing"
    assert (sb["feature_incompat"] & ~EXT2_FEATURE_INCOMPAT_FILETYPE) == 0, \
        "unexpected incompat features 0x%x" % sb["feature_incompat"]

    g = parse_group_desc(img, sb, 0)
    assert g["block_bitmap"] < sb["blocks_count"], "block bitmap block out of range"
    assert g["inode_bitmap"] < sb["blocks_count"], "inode bitmap block out of range"
    assert g["inode_table"] < sb["blocks_count"], "inode table block out of range"

    # ---- bitmap cross-checks vs. superblock/group counters ----
    bs = sb["block_size"]
    bm = img[g["block_bitmap"] * bs:(g["block_bitmap"] + 1) * bs]
    used_real = 0
    for b in range(sb["blocks_count"]):
        if bm[b // 8] & (1 << (b % 8)):
            used_real += 1
    for b in range(sb["blocks_count"], sb["blocks_per_group"]):
        assert bm[b // 8] & (1 << (b % 8)), "padding bit %d must be marked used" % b
    assert used_real == sb["blocks_count"] - sb["free_blocks_count"], \
        "block bitmap: %d used but superblock says %d" % (
            used_real, sb["blocks_count"] - sb["free_blocks_count"])
    assert g["free_blocks_count"] == sb["free_blocks_count"], "group free blocks mismatch"

    im = img[g["inode_bitmap"] * bs:(g["inode_bitmap"] + 1) * bs]
    used_inodes = 0
    for i in range(sb["inodes_count"]):
        if im[i // 8] & (1 << (i % 8)):
            used_inodes += 1
    assert used_inodes == sb["inodes_count"] - sb["free_inodes_count"], \
        "inode bitmap: %d used but superblock says %d" % (
            used_inodes, sb["inodes_count"] - sb["free_inodes_count"])
    assert g["free_inodes_count"] == sb["free_inodes_count"], "group free inodes mismatch"

    # ---- root directory ----
    root = parse_inode(img, sb, g, INO_ROOT)
    assert (root["mode"] & 0o170000) == 0o040000, "inode 2 is not a directory"
    root_entries = walk_dir(img, sb, root)
    dot_ino, dot_ft = find_entry(root_entries, b".")
    dotdot_ino, _ = find_entry(root_entries, b"..")
    assert dot_ino == INO_ROOT and dot_ft == FT_DIR, "root '.' entry broken"
    assert dotdot_ino == INO_ROOT, "root '..' entry broken"

    # ---- every file: path walk + byte-for-byte content ----
    for path, want in info["expect"].items():
        parts = [p for p in path.split("/") if p]
        cur_ino, cur_mode = INO_ROOT, 0o040000
        cur_dir_entries = root_entries
        for idx, part in enumerate(parts):
            assert (cur_mode & 0o170000) == 0o040000, "%s: %r is not a directory" % (path, part)
            ino, ft = find_entry(cur_dir_entries, part.encode())
            assert ino is not None, "%s: component %r not found" % (path, part)
            inode = parse_inode(img, sb, g, ino)
            cur_ino, cur_mode = ino, inode["mode"]
            if idx < len(parts) - 1:
                assert (cur_mode & 0o170000) == 0o040000, "%s: %r is not a dir" % (path, part)
                cur_dir_entries = walk_dir(img, sb, inode)
            else:
                assert (cur_mode & 0o170000) == 0o100000, "%s is not a regular file" % path
                got = read_file_bytes(img, sb, inode)
                assert len(got) == len(want), "%s: size %d != %d" % (path, len(got), len(want))
                assert got == want, "%s: content mismatch" % path
                assert inode["links"] == 1, "%s: links %d" % (path, inode["links"])
                exp_sectors = sum(
                    1 for lb in range((len(want) + bs - 1) // bs)
                    if resolve_block(img, sb, inode, lb) != 0)
                assert inode["blocks"] >= exp_sectors * (bs // SECTOR), \
                    "%s: i_blocks %d too small" % (path, inode["blocks"])

    # ---- subdirectory ----
    sub_ino, sub_ft = find_entry(root_entries, b"SUB")
    assert sub_ino == INO_SUB and sub_ft == FT_DIR, "SUB missing or wrong type"
    sub = parse_inode(img, sb, g, INO_SUB)
    sub_entries = walk_dir(img, sb, sub)
    s_dot, _ = find_entry(sub_entries, b".")
    s_dotdot, _ = find_entry(sub_entries, b"..")
    assert s_dot == INO_SUB, "SUB '.' entry broken"
    assert s_dotdot == INO_ROOT, "SUB '..' entry broken"
    assert find_entry(sub_entries, b"NESTED.TXT")[0] == INO_NESTED, "NESTED.TXT missing"

    # ---- explicit indirection-level assertions ----
    big = parse_inode(img, sb, g, INO_BIG)
    assert big["block"][12] != 0, "BIG.BIN must use the single-indirect block"
    assert big["block"][13] == 0, "BIG.BIN must not need double indirect"
    dbl = parse_inode(img, sb, g, INO_DOUBLE)
    ptrs = bs // 4
    if (DOUBLE_BIN_SIZE + bs - 1) // bs > 12 + ptrs:
        assert dbl["block"][13] != 0, "DOUBLE.BIN must use the double-indirect tree"
    sparse = parse_inode(img, sb, g, INO_SPARSE)
    assert sparse["block"][SPARSE_HOLE_BLOCK] == 0, "SPARSE.BIN hole must be a 0 pointer"
    assert sparse["block"][0] != 0 and sparse["block"][2] != 0, "SPARSE.BIN data blocks missing"
    assert sparse["size"] == SPARSE_BIN_BLOCKS * bs, "SPARSE.BIN size"

    return {"sb": sb, "gdt": g, "root_entries": root_entries, "sub_entries": sub_entries}


def describe_indirection(bs, nblocks):
    ptrs = bs // 4
    if nblocks <= 12:
        return "direct"
    if nblocks <= 12 + ptrs:
        return "single-indirect"
    if nblocks <= 12 + ptrs + ptrs * ptrs:
        return "double-indirect"
    return "triple-indirect"


# ==========================================================================
# Synthetic TRIPLE-INDIRECT conformance fixture (opt-in: --triple-fixture).
#
# A 4 MiB volume cannot reach the triple-indirect region through a *realistic*
# file (that needs > 64 MiB of logical blocks), so this image is deliberately
# synthetic: ONE file /TRIPLE.BIN whose i_size spans far past the volume, with
# exactly four mapped logical blocks - one reached through each level:
#
#   logical 0                        -> i_block[0]                 (direct)
#   logical 12                       -> i_block[12] -> table       (single)
#   logical 12+ptrs                  -> i_block[13] -> 2 levels    (double)
#   logical 12+ptrs+ptrs*ptrs        -> i_block[14] -> 3 levels    (triple)
#
# Everything else in the file is a hole (zeros).  It is NOT a normal test
# image: no debugfs/fsck would bless a file larger than its volume.  It exists
# to exercise the driver's bounded triple-indirect walk from the host side.
#
# Pattern rule for /TRIPLE.BIN, byte i inside logical block L:
#   b[i] = (i + L * 13 + 1) & 0xFF
# ==========================================================================

def triple_pattern(logical, block_size):
    return bytes(((i + logical * 13 + 1) & 0xFF) for i in range(block_size))


def build_triple_fixture(block_size=1024):
    total_blocks = IMG_SIZE // block_size
    ptrs = block_size // 4
    b = ImageBuilder(block_size, total_blocks)

    root_blk = b.alloc(1)

    logical_single = 12
    logical_double = 12 + ptrs
    logical_triple = 12 + ptrs + ptrs * ptrs

    d0 = b.alloc(1)                                     # direct data
    s1 = b.alloc(1); sd = b.alloc(1)                     # single indirect
    d1a = b.alloc(1); d1b = b.alloc(1); dd = b.alloc(1)  # double indirect
    t1 = b.alloc(1); t2 = b.alloc(1); t3 = b.alloc(1); td = b.alloc(1)  # triple

    b.write_block(d0, triple_pattern(0, block_size))
    b.write_block(sd, triple_pattern(logical_single, block_size))
    b.write_block(dd, triple_pattern(logical_double, block_size))
    b.write_block(td, triple_pattern(logical_triple, block_size))

    st = [0] * ptrs
    st[0] = sd
    b.put_pointer_table(s1, st)
    dt = [0] * ptrs
    dt[0] = d1b
    b.put_pointer_table(d1a, dt)
    ct = [0] * ptrs
    ct[0] = dd
    b.put_pointer_table(d1b, ct)
    b.put_pointer_table(t1, [t2] + [0] * (ptrs - 1))
    b.put_pointer_table(t2, [t3] + [0] * (ptrs - 1))
    b.put_pointer_table(t3, [td] + [0] * (ptrs - 1))

    i_block = [0] * 15
    i_block[0] = d0
    i_block[12] = s1
    i_block[13] = d1a
    i_block[14] = t1
    file_size = (logical_triple + 1) * block_size
    b.inodes[11] = {
        "mode": 0o100644,
        "size": file_size,
        "links": 1,
        "ptrs": i_block,
        "blocks": 4 * (block_size // SECTOR),
    }

    b.write_block(root_blk, build_dir_block([
        (2, b".", FT_DIR),
        (2, b"..", FT_DIR),
        (11, b"TRIPLE.BIN", FT_REG),
    ], block_size))
    b.inodes[2] = {"mode": 0o040755, "size": block_size, "links": 2,
                   "ptrs": [root_blk] + [0] * 14, "blocks": block_size // SECTOR}

    b.write_inode_table()
    b.write_bitmaps()
    b.write_superblock()
    b.write_gdt()

    info = {
        "block_size": block_size,
        "logical": {"direct": 0, "single": logical_single, "double": logical_double,
                    "triple": logical_triple},
        "size": file_size,
        "physical": {"direct": d0, "single": sd, "double": dd, "triple": td,
                     "single_table": s1, "double_top": d1a, "double_child": d1b,
                     "triple_l1": t1, "triple_l2": t2, "triple_l3": t3},
    }
    return bytes(b.img), info


def verify_triple_fixture(img, info):
    """Independent walk of the triple-indirect chain (explicit index math)."""
    bs = info["block_size"]
    ptrs = bs // 4
    sb = parse_superblock(img)
    assert sb["magic"] == EXT2_MAGIC
    assert sb["blocks_count"] * sb["block_size"] == len(img)
    g = parse_group_desc(img, sb, 0)
    inode = parse_inode(img, sb, g, 11)
    assert inode["size"] == info["size"], "TRIPLE.BIN size"
    assert inode["block"][14] != 0, "i_block[14] must be set"

    def table(blk):
        raw = img[blk * bs:(blk + 1) * bs]
        assert len(raw) == bs, "table block %d outside image" % blk
        return [struct.unpack_from("<I", raw, k * 4)[0] for k in range(ptrs)]

    # single indirect
    assert inode["block"][12] == info["physical"]["single_table"]
    t = table(inode["block"][12])
    assert t[0] == info["physical"]["single"], "single-indirect child"
    # double indirect: top -> child -> data
    assert inode["block"][13] == info["physical"]["double_top"]
    top = table(inode["block"][13])
    assert top[0] == info["physical"]["double_child"]
    child = table(top[0])
    assert child[0] == info["physical"]["double"], "double-indirect child"
    # triple indirect: L1 -> L2 -> L3 -> data
    assert inode["block"][14] == info["physical"]["triple_l1"]
    l1 = table(inode["block"][14])
    assert l1[0] == info["physical"]["triple_l2"], "triple level 1"
    l2 = table(l1[0])
    assert l2[0] == info["physical"]["triple_l3"], "triple level 2"
    l3 = table(l2[0])
    assert l3[0] == info["physical"]["triple"], "triple level 3"

    # every mapped logical block must resolve through the expected level and
    # carry the documented pattern; sample holes must read as zero (no I/O)
    for name in ("direct", "single", "double", "triple"):
        logical = info["logical"][name]
        phys = resolve_block(img, sb, inode, logical)
        assert phys == info["physical"][name], \
            "%s: logical %d -> %d (want %d)" % (name, logical, phys,
                                                info["physical"][name])
        want = triple_pattern(logical, bs)
        got = img[phys * bs:(phys + 1) * bs]
        assert got == want, "%s block content mismatch" % name
    for hole in (1, 11, 13, info["logical"]["single"] + 1,
                 info["logical"]["double"] + 1, info["logical"]["triple"] - 1):
        assert resolve_block(img, sb, inode, hole) == 0, "logical %d must be a hole" % hole

    # directory lookup of /TRIPLE.BIN
    root_entries = walk_dir(img, sb, parse_inode(img, sb, g, 2))
    ino, ft = find_entry(root_entries, b"TRIPLE.BIN")
    assert ino == 11 and ft == FT_REG, "/TRIPLE.BIN entry"
    return True


def build_one(block_size, out_path):
    img, info = build_image(block_size)
    v = verify_image(img, info)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(img)
    sb = v["sb"]
    print("ext2 image  : %s (%d bytes)" % (out_path, len(img)))
    print("  block size: %d (log=%d)  blocks=%d  inodes=%d  groups=%d  rev=%d  state=0x%x" % (
        sb["block_size"], sb["log_block_size"], sb["blocks_count"], sb["inodes_count"],
        1, sb["rev_level"], sb["state"]))
    print("  used/free : %d/%d blocks, %d free inodes" % (
        info["used_blocks"], info["free_blocks"], info["free_inodes"]))
    for path in ("/HELLO.TXT", "/BIG.BIN", "/SUB/NESTED.TXT", "/SPARSE.BIN", "/DOUBLE.BIN"):
        nblocks = (len(info["expect"][path]) + block_size - 1) // block_size
        extra = ""
        if path == "/SPARSE.BIN":
            extra = ", hole at logical block %d" % SPARSE_HOLE_BLOCK
        print("  %-16s %7d B  %3d blocks  %s%s" % (
            path, len(info["expect"][path]), nblocks,
            describe_indirection(block_size, nblocks), extra))
    print("host-side verification: PASS")
    return img, info


def build_triple_image(block_size, out_path):
    img, info = build_triple_fixture(block_size)
    verify_triple_fixture(img, info)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(img)
    print("ext2 image  : %s (%d bytes)" % (out_path, len(img)))
    print("  block size: %d  blocks=%d  (synthetic triple-indirect fixture)"
          % (block_size, len(img) // block_size))
    print("  /TRIPLE.BIN size=%d bytes" % info["size"])
    for name in ("direct", "single", "double", "triple"):
        print("    logical %-9d -> physical %-6d %s" % (
            info["logical"][name], info["physical"][name], name))
    print("host-side verification: PASS")
    return img, info


def main():
    ap = argparse.ArgumentParser(description="build real ext2 rev1 test images")
    ap.add_argument("--blocksize", type=int, default=None, choices=(1024, 2048, 4096),
                    help="build only this block size (default: all three images)")
    ap.add_argument("--out", default=None,
                    help="output path for --blocksize (default build/ext2.img)")
    args = ap.parse_args()

    if args.blocksize:
        out = args.out or os.path.join(OUT_DIR, "ext2.img")
        build_one(args.blocksize, out)
    else:
        build_one(1024, os.path.join(OUT_DIR, "ext2.img"))
        print("")
        build_one(4096, os.path.join(OUT_DIR, "ext2_4k.img"))
        print("")
        build_triple_image(1024, os.path.join(OUT_DIR, "ext2_triple.img"))
        print("")

    print("all images: host-side verification PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
