#!/usr/bin/env python3
"""SutureOS ext4 read-only test image generator (pure standard library).

Builds a 16 MiB ext4 volume that the SutureOS ext4 driver (kernel/fs/ext4.c)
must be able to mount read-only and read:

    block size      4096
    inode size      256
    block groups    1            (flex_bg off)
    features        incompat = extents(0x0040) | filetype(0x0002)
                    ro_compat = sparse_super(0x0001)
                    compat   = none
    NOT enabled     metadata_csum, 64bit, mmp, bigalloc, inline_data,
                    dir_index, has_journal, huge_file, quota, ...

Content (all recorded in build/ext4_manifest.json with size + sha256):

    /HELLO.TXT        short text file, 1 extent
    /BIG.BIN          1.5 MiB pattern file whose data blocks are deliberately
                      interleaved with /SUBDIR/FRAG.BIN, so the extent tree is
                      DEPTH 1 with TWO leaf blocks (384 one-block extents)
    /SUBDIR/          a real subdirectory
    /SUBDIR/INNER.TXT nested text file (dir traversal + separate extent tree)
    /SUBDIR/FRAG.BIN  1.5 MiB filler that forces the fragmentation above
    /EMPTY            0-byte file (empty extent tree)
    /SPARSE.BIN       3 blocks, logical block 1 is a HOLE (no extent covers it)
    /NOTE4K.TXT       exactly 4096 bytes of ASCII (safe for `cat` in the shell)
    /LINK.TXT         FAST symlink to /HELLO.TXT (target stored in i_block)

`--verify` re-parses the image with a READER THAT SHARES NO CODE WITH THE
WRITER (it re-derives every geometry value from the superblock bytes and walks
the extent trees / directories / inodes from scratch). It is the generator's
own consistency check; scripts/verify_ext4_read.py is the third, fully separate
implementation used against the image QEMU has actually read.

Usage:
    py -3 scripts/mkfs_ext4_testimg.py            # build + write + verify
    py -3 scripts/mkfs_ext4_testimg.py --verify   # verify the existing image
"""
import argparse
import hashlib
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "build")
IMG_PATH = os.path.join(OUT_DIR, "ext4.img")
MANIFEST_PATH = os.path.join(OUT_DIR, "ext4_manifest.json")

# ---------------------------------------------------------------- constants --
BS = 4096                      # filesystem block size
TOTAL_BLOCKS = 4096            # 16 MiB
INODE_SIZE = 256
INODES = 64
INODES_PER_GROUP = 64
BLOCKS_PER_GROUP = 32768       # mke2fs default for 4 KiB blocks
FIRST_DATA_BLOCK = 0
MKFS_TIME = 1700000000         # fixed timestamps => reproducible image

SUPER_MAGIC = 0xEF53
INCOMPAT_FILETYPE = 0x0002
INCOMPAT_EXTENTS = 0x0040
RO_COMPAT_SPARSE_SUPER = 0x0001
EXTENTS_FL = 0x00080000
EXTENT_MAGIC = 0xF30A

S_IFREG = 0o100000
S_IFDIR = 0o040000
S_IFLNK = 0o120000

FT_REG = 1
FT_DIR = 2
FT_SYMLINK = 7

BLK_SB = 0                     # superblock lives at byte offset 1024 of block 0
BLK_GDT = 1                    # primary group descriptor table (1 group => 1 block)
BLK_BLOCK_BITMAP = 2
BLK_INODE_BITMAP = 3
BLK_INODE_TABLE = 4            # 64 inodes * 256 B = 4 blocks
INODE_TABLE_BLOCKS = (INODES * INODE_SIZE) // BS

# /BIG.BIN is 1.5 MiB = 384 blocks. Its blocks are interleaved 1:1 with
# /SUBDIR/FRAG.BIN, so every block is its own extent => 384 extents, which no
# single leaf block can hold ((4096-12)/12 = 340), hence a depth-1 tree with
# exactly two leaf blocks. That is what makes the driver's index traversal
# observable instead of theoretical.
BIG_BLOCKS = 384

HELLO_TEXT = (b"Hello from SutureOS ext4!\n"
              b"This file lives on a real ext4 volume: extent tree + "
              b"filetype dirents, read by a read-only driver.\n")
INNER_TEXT = (b"Nested file: SUBDIR/INNER.TXT reached through the ext4 "
              b"directory tree.\n")
LINK_TARGET = b"/HELLO.TXT"


def big_pattern(i):
    return (i * 37 + 11) & 0xFF


def frag_pattern(i):
    return (i * 43 + 19) & 0xFF


def sparse_pattern_a(i):
    return (i * 17 + 3) & 0xFF


def sparse_pattern_b(i):
    return (i * 23 + 5) & 0xFF


def note4k_bytes():
    line = b"NOTE4K.TXT SutureOS ext4 read path: extent tree, 4096-byte blocks.\n"
    buf = bytearray()
    while len(buf) < BS:
        buf += line
    buf = buf[:BS]
    buf[BS - 1] = ord("\n")
    return bytes(buf)


# ===========================================================================
# PART 1 - the writer
# ===========================================================================

def put_u16(buf, off, val):
    struct.pack_into("<H", buf, off, val & 0xFFFF)


def put_u32(buf, off, val):
    struct.pack_into("<I", buf, off, val & 0xFFFFFFFF)


def extent_header(entries, max_entries, depth):
    return struct.pack("<HHHHI", EXTENT_MAGIC, entries, max_entries, depth, 0)


def extent_entry(logical, length, start):
    return struct.pack("<IHHI", logical, length, (start >> 32) & 0xFFFF,
                       start & 0xFFFFFFFF)


def index_entry(logical, leaf):
    return struct.pack("<IIHH", logical, leaf & 0xFFFFFFFF, (leaf >> 32) & 0xFFFF, 0)


def build_extent_leaf(extents):
    """extents: list of (logical, length, start) already sorted by logical."""
    max_entries = (BS - 12) // 12
    body = bytearray(extent_header(len(extents), max_entries, 0))
    for e in extents:
        body += extent_entry(*e)
    assert len(body) <= BS, "extent leaf overflows one block"
    body += b"\x00" * (BS - len(body))
    return bytes(body), max_entries


def build_dir_block(entries):
    """entries: [(name_bytes, ino, file_type)]; last entry stretches to block end."""
    data = bytearray()
    for idx, (name, ino, ftype) in enumerate(entries):
        ent_len = 8 + len(name)
        min_len = (ent_len + 3) & ~3
        if idx == len(entries) - 1:
            rec = BS - len(data)
        else:
            rec = min_len
        assert rec >= min_len
        data += struct.pack("<IHBB", ino, rec, len(name), ftype)
        data += name
        data += b"\x00" * (rec - ent_len)
    assert len(data) == BS, "directory block must fill exactly one block"
    return bytes(data)


def build_inode(mode, size, links, blocks_512, iblock, flags):
    b = bytearray(INODE_SIZE)
    put_u16(b, 0, mode)
    put_u16(b, 2, 0)                    # i_uid
    put_u32(b, 4, size & 0xFFFFFFFF)    # i_size_lo
    put_u32(b, 8, MKFS_TIME)            # i_atime
    put_u32(b, 12, MKFS_TIME)           # i_ctime
    put_u32(b, 16, MKFS_TIME)           # i_mtime
    put_u32(b, 20, 0)                   # i_dtime
    put_u16(b, 24, 0)                   # i_gid
    put_u16(b, 26, links)
    put_u32(b, 28, blocks_512)          # i_blocks_lo (512-byte units)
    put_u32(b, 32, flags)
    put_u32(b, 36, 0)                   # i_osd1
    assert len(iblock) == 60
    b[40:100] = iblock
    put_u32(b, 100, 0)                  # i_generation
    put_u32(b, 104, 0)                  # i_file_acl_lo
    put_u32(b, 108, size >> 32)         # i_size_high
    put_u32(b, 112, 0)                  # i_obso_faddr
    put_u16(b, 128, 32)                 # i_extra_isize (standard 32)
    return bytes(b)


def build_superblock(free_blocks, free_inodes):
    sb = bytearray(1024)
    put_u32(sb, 0, INODES)                  # s_inodes_count
    put_u32(sb, 4, TOTAL_BLOCKS)            # s_blocks_count_lo
    put_u32(sb, 8, 0)                       # s_r_blocks_count_lo
    put_u32(sb, 12, free_blocks)            # s_free_blocks_count_lo
    put_u32(sb, 16, free_inodes)            # s_free_inodes_count
    put_u32(sb, 20, FIRST_DATA_BLOCK)       # s_first_data_block
    put_u32(sb, 24, 2)                      # s_log_block_size: 1024 << 2 = 4096
    put_u32(sb, 28, 2)                      # s_log_cluster_size (== block size)
    put_u32(sb, 32, BLOCKS_PER_GROUP)       # s_blocks_per_group
    put_u32(sb, 36, BLOCKS_PER_GROUP)       # s_clusters_per_group
    put_u32(sb, 40, INODES_PER_GROUP)       # s_inodes_per_group
    put_u32(sb, 44, 0)                      # s_mtime
    put_u32(sb, 48, MKFS_TIME)              # s_wtime
    put_u16(sb, 52, 0)                      # s_mnt_count
    put_u16(sb, 54, 0xFFFF)                 # s_max_mnt_count
    put_u16(sb, 56, SUPER_MAGIC)            # s_magic
    put_u16(sb, 58, 0x0001)                 # s_state: cleanly unmounted
    put_u16(sb, 60, 1)                      # s_errors: continue
    put_u16(sb, 62, 0)                      # s_minor_rev_level
    put_u32(sb, 64, MKFS_TIME)              # s_lastcheck
    put_u32(sb, 68, 0)                      # s_checkinterval
    put_u32(sb, 72, 0)                      # s_creator_os: Linux
    put_u32(sb, 76, 1)                      # s_rev_level: DYNAMIC_REV (ext4)
    put_u16(sb, 80, 0)                      # s_def_resuid
    put_u16(sb, 82, 0)                      # s_def_resgid
    put_u32(sb, 84, 11)                     # s_first_ino
    put_u16(sb, 88, INODE_SIZE)             # s_inode_size
    put_u16(sb, 90, 0)                      # s_block_group_nr
    put_u32(sb, 92, 0)                      # s_feature_compat: none
    put_u32(sb, 96, INCOMPAT_FILETYPE | INCOMPAT_EXTENTS)
    put_u32(sb, 100, RO_COMPAT_SPARSE_SUPER)
    sb[104:120] = bytes(range(0x40, 0x50))  # s_uuid (fixed, reproducible)
    vol = b"sutureos-e4"
    sb[120:120 + len(vol)] = vol            # s_volume_name[16]
    put_u32(sb, 200, 0)                     # s_algorithm_usage_bitmap
    sb[204] = 0                             # s_prealloc_blocks
    sb[205] = 0                             # s_prealloc_dir_blocks
    put_u16(sb, 206, 0)                     # s_reserved_gdt_blocks
    put_u32(sb, 224, 0)                     # s_journal_inum
    put_u32(sb, 228, 0)                     # s_journal_dev
    put_u32(sb, 232, 0)                     # s_last_orphan
    sb[252] = 1                             # s_def_hash_version (half_md4)
    sb[253] = 0                             # s_jnl_backup_type
    put_u16(sb, 254, 0)                     # s_desc_size: 32 (64bit off)
    # s_blocks_count_hi / free_blocks_hi stay 0 (no 64bit feature)
    return bytes(sb)


def build_group_desc(free_blocks, free_inodes, used_dirs):
    gd = bytearray(32)
    put_u32(gd, 0, BLK_BLOCK_BITMAP)
    put_u32(gd, 4, BLK_INODE_BITMAP)
    put_u32(gd, 8, BLK_INODE_TABLE)
    put_u16(gd, 12, free_blocks)
    put_u16(gd, 14, free_inodes)
    put_u16(gd, 16, used_dirs)
    put_u16(gd, 18, 0)                      # bg_flags
    put_u32(gd, 20, 0)                      # bg_exclude_bitmap_lo
    return bytes(gd)


def build_image():
    img = bytearray(TOTAL_BLOCKS * BS)
    used_blocks = set()

    def claim(start, count=1):
        for b in range(start, start + count):
            assert b not in used_blocks, "double allocation of block %d" % b
            used_blocks.add(b)

    claim(BLK_SB)
    claim(BLK_GDT)
    claim(BLK_BLOCK_BITMAP)
    claim(BLK_INODE_BITMAP)
    claim(BLK_INODE_TABLE, INODE_TABLE_BLOCKS)

    # ---- allocate the data blocks -----------------------------------------
    cursor = [BLK_INODE_TABLE + INODE_TABLE_BLOCKS]

    def take(count=1):
        start = cursor[0]
        cursor[0] += count
        claim(start, count)
        return start

    root_blk = take()
    sub_blk = take()
    hello_blk = take()
    inner_blk = take()
    note_blk = take()
    sparse_a = take()
    sparse_b = take()

    big_blocks = []
    frag_blocks = []
    for _ in range(BIG_BLOCKS):
        big_blocks.append(take())
        frag_blocks.append(take())

    big_leaf_blocks = [take(), take()]
    frag_leaf_blocks = [take(), take()]

    # ---- file payloads -----------------------------------------------------
    big_size = len(big_blocks) * BS
    frag_size = len(frag_blocks) * BS
    big_data = bytes(big_pattern(i) for i in range(big_size))
    frag_data = bytes(frag_pattern(i) for i in range(frag_size))
    note_data = note4k_bytes()
    assert len(note_data) == BS

    # ---- extents -----------------------------------------------------------
    # Small files: one extent each (depth 0).
    def single(logical0, blk, length=1):
        return [(logical0, length, blk)], 0

    hello_ext, _ = single(0, hello_blk)
    inner_ext, _ = single(0, inner_blk)
    note_ext, _ = single(0, note_blk)
    root_ext, _ = single(0, root_blk)
    sub_ext, _ = single(0, sub_blk)
    sparse_exts = [(0, 1, sparse_a), (2, 1, sparse_b)]     # logical block 1 = hole

    # BIG/FRAG: one extent per interleaved block => depth 1, two leaves.
    big_exts = [(i, 1, blk) for i, blk in enumerate(big_blocks)]
    frag_exts = [(i, 1, blk) for i, blk in enumerate(frag_blocks)]

    def tree_for(exts):
        if len(exts) <= 4:
            root = bytearray(extent_header(len(exts), 4, 0))
            for e in exts:
                root += extent_entry(*e)
            root += b"\x00" * (60 - len(root))
            return bytes(root), {}, 0, len(exts)
        leaf_capacity = (BS - 12) // 12
        chunks = [exts[i:i + leaf_capacity] for i in range(0, len(exts), leaf_capacity)]
        leaves = {}
        idx = []
        for chunk in chunks:
            body, _mx = build_extent_leaf(chunk)
            idx.append((chunk[0][0], body))
        root = bytearray(extent_header(len(idx), 4, 1))
        for logical, _body in idx:
            root += index_entry(logical, 0)         # physical leaf filled below
        root += b"\x00" * (60 - len(root))
        return bytes(root), idx, 1, len(exts)

    big_root, big_leaves, big_depth, big_n = tree_for(big_exts)
    frag_root, frag_leaves, frag_depth, frag_n = tree_for(frag_exts)

    # Patch the leaf pointers into the root now that the blocks are known.
    if big_depth == 1:
        # rewrite index logical blocks properly (patch_root keeps them)
        b = bytearray(big_root)
        for i, (logical, _body) in enumerate(big_leaves):
            struct.pack_into("<IIHH", b, 12 + i * 12, logical,
                             big_leaf_blocks[i] & 0xFFFFFFFF, 0, 0)
        big_root = bytes(b)
    if frag_depth == 1:
        b = bytearray(frag_root)
        for i, (logical, _body) in enumerate(frag_leaves):
            struct.pack_into("<IIHH", b, 12 + i * 12, logical,
                             frag_leaf_blocks[i] & 0xFFFFFFFF, 0, 0)
        frag_root = bytes(b)

    # ---- inode numbers -----------------------------------------------------
    INO_HELLO = 11
    INO_BIG = 12
    INO_SUB = 13
    INO_INNER = 14
    INO_EMPTY = 15
    INO_FRAG = 16
    INO_SPARSE = 17
    INO_NOTE = 18
    INO_LINK = 19

    # ---- directories (filetype format: name_len u8 + file_type u8) ---------
    root_entries = [
        (b".", 2, FT_DIR),
        (b"..", 2, FT_DIR),
        (b"HELLO.TXT", INO_HELLO, FT_REG),
        (b"BIG.BIN", INO_BIG, FT_REG),
        (b"SUBDIR", INO_SUB, FT_DIR),
        (b"EMPTY", INO_EMPTY, FT_REG),
        (b"SPARSE.BIN", INO_SPARSE, FT_REG),
        (b"NOTE4K.TXT", INO_NOTE, FT_REG),
        (b"LINK.TXT", INO_LINK, FT_SYMLINK),
    ]
    sub_entries = [
        (b".", INO_SUB, FT_DIR),
        (b"..", 2, FT_DIR),
        (b"INNER.TXT", INO_INNER, FT_REG),
        (b"FRAG.BIN", INO_FRAG, FT_REG),
    ]

    # ---- write everything --------------------------------------------------
    def write_block(blk, data):
        assert len(data) == BS, "block payload must be %d bytes" % BS
        img[blk * BS:(blk + 1) * BS] = data

    write_block(root_blk, build_dir_block(root_entries))
    write_block(sub_blk, build_dir_block(sub_entries))
    write_block(hello_blk, HELLO_TEXT + b"\x00" * (BS - len(HELLO_TEXT)))
    write_block(inner_blk, INNER_TEXT + b"\x00" * (BS - len(INNER_TEXT)))
    write_block(note_blk, note_data)
    write_block(sparse_a, bytes(sparse_pattern_a(i) for i in range(BS)))
    write_block(sparse_b, bytes(sparse_pattern_b(i) for i in range(BS)))

    for i, blk in enumerate(big_blocks):
        off = i * BS
        write_block(blk, big_data[off:off + BS])
    for i, blk in enumerate(frag_blocks):
        off = i * BS
        write_block(blk, frag_data[off:off + BS])

    if big_depth == 1:
        leaf_capacity = (BS - 12) // 12
        for i, (_logical, body) in enumerate(big_leaves):
            write_block(big_leaf_blocks[i], body)
    if frag_depth == 1:
        for i, (_logical, body) in enumerate(frag_leaves):
            write_block(frag_leaf_blocks[i], body)

    # ---- inodes ------------------------------------------------------------
    def inode_at(ino, blob):
        base = BLK_INODE_TABLE * BS + (ino - 1) * INODE_SIZE
        img[base:base + INODE_SIZE] = blob

    empty_root = bytearray(extent_header(0, 4, 0))
    empty_root += b"\x00" * (60 - len(empty_root))
    root_nlink = 2 + sum(1 for n, _i, t in root_entries if t == FT_DIR and n not in (b".", b".."))
    sub_nlink = 2

    # inode 1: bad-blocks inode (reserved, unused)
    inode_at(1, build_inode(0, 0, 0, 0, b"\x00" * 60, 0))
    # inode 2: root directory
    inode_at(2, build_inode(S_IFDIR | 0o755, BS, root_nlink, 1 * (BS // 512),
                            tree_bytes_for(root_ext), EXTENTS_FL))
    inode_at(INO_HELLO, build_inode(S_IFREG | 0o644, len(HELLO_TEXT), 1,
                                    (BS // 512), tree_bytes_for(hello_ext), EXTENTS_FL))
    inode_at(INO_BIG, build_inode(S_IFREG | 0o644, big_size, 1,
                                  (len(big_blocks) + (len(big_leaf_blocks) if big_depth else 0)) *
                                  (BS // 512), big_root, EXTENTS_FL))
    inode_at(INO_SUB, build_inode(S_IFDIR | 0o755, BS, sub_nlink, (BS // 512),
                                  tree_bytes_for(sub_ext), EXTENTS_FL))
    inode_at(INO_INNER, build_inode(S_IFREG | 0o644, len(INNER_TEXT), 1,
                                    (BS // 512), tree_bytes_for(inner_ext), EXTENTS_FL))
    inode_at(INO_EMPTY, build_inode(S_IFREG | 0o644, 0, 1, 0, bytes(empty_root), EXTENTS_FL))
    inode_at(INO_FRAG, build_inode(S_IFREG | 0o644, frag_size, 1,
                                   (len(frag_blocks) + (len(frag_leaf_blocks) if frag_depth else 0)) *
                                   (BS // 512), frag_root, EXTENTS_FL))
    inode_at(INO_SPARSE, build_inode(S_IFREG | 0o644, 3 * BS, 1, (2 * BS) // 512,
                                     tree_bytes_for(sparse_exts), EXTENTS_FL))
    inode_at(INO_NOTE, build_inode(S_IFREG | 0o644, len(note_data), 1, (BS // 512),
                                   tree_bytes_for(note_ext), EXTENTS_FL))
    # FAST symlink: the target bytes live in i_block, no extents, no data blocks.
    link_iblock = bytearray(60)
    link_iblock[:len(LINK_TARGET)] = LINK_TARGET
    inode_at(INO_LINK, build_inode(S_IFLNK | 0o777, len(LINK_TARGET), 1, 0,
                                   bytes(link_iblock), 0))

    # ---- bitmaps -----------------------------------------------------------
    block_bitmap = bytearray(BS)
    for b in used_blocks:
        block_bitmap[b // 8] |= 1 << (b % 8)
    # Bits past s_blocks_count within the group are marked used (mke2fs does the
    # same) so a bitmap scan cannot "find" blocks that do not exist.
    for b in range(TOTAL_BLOCKS, BLOCKS_PER_GROUP):
        block_bitmap[b // 8] |= 1 << (b % 8)
    img[BLK_BLOCK_BITMAP * BS:(BLK_BLOCK_BITMAP + 1) * BS] = block_bitmap

    used_inodes = {1, 2, INO_HELLO, INO_BIG, INO_SUB, INO_INNER, INO_EMPTY,
                   INO_FRAG, INO_SPARSE, INO_NOTE, INO_LINK}
    inode_bitmap = bytearray(BS)
    for ino in used_inodes:
        inode_bitmap[(ino - 1) // 8] |= 1 << ((ino - 1) % 8)
    img[BLK_INODE_BITMAP * BS:(BLK_INODE_BITMAP + 1) * BS] = inode_bitmap

    free_blocks = TOTAL_BLOCKS - len(used_blocks)
    free_inodes = INODES - len(used_inodes)
    used_dirs = 2   # root + SUBDIR

    img[1024:1024 + 1024] = build_superblock(free_blocks, free_inodes)
    gd = build_group_desc(free_blocks, free_inodes, used_dirs)
    img[BLK_GDT * BS:BLK_GDT * BS + 32] = gd

    # ---- manifest facts ----------------------------------------------------
    files = [
        {"path": "/HELLO.TXT", "type": "file", "inode": INO_HELLO,
         "size": len(HELLO_TEXT), "sha256": sha256(HELLO_TEXT),
         "extents": 1, "depth": 0},
        {"path": "/BIG.BIN", "type": "file", "inode": INO_BIG,
         "size": big_size, "sha256": sha256(big_data),
         "extents": big_n, "depth": big_depth,
         "leaves": len(big_leaf_blocks) if big_depth else 0,
         "first_block": big_blocks[0], "stride": 2,
         "note": "interleaved with /SUBDIR/FRAG.BIN: one extent per block"},
        {"path": "/SUBDIR", "type": "dir", "inode": INO_SUB, "size": BS,
         "links": sub_nlink, "extents": 1, "depth": 0},
        {"path": "/SUBDIR/INNER.TXT", "type": "file", "inode": INO_INNER,
         "size": len(INNER_TEXT), "sha256": sha256(INNER_TEXT),
         "extents": 1, "depth": 0},
        {"path": "/SUBDIR/FRAG.BIN", "type": "file", "inode": INO_FRAG,
         "size": frag_size, "sha256": sha256(frag_data),
         "extents": frag_n, "depth": frag_depth,
         "leaves": len(frag_leaf_blocks) if frag_depth else 0},
        {"path": "/EMPTY", "type": "file", "inode": INO_EMPTY,
         "size": 0, "sha256": sha256(b""), "extents": 0, "depth": 0},
        {"path": "/SPARSE.BIN", "type": "file", "inode": INO_SPARSE,
         "size": 3 * BS,
         "sha256": sha256(bytes(sparse_pattern_a(i) for i in range(BS)) + bytes(BS)
                          + bytes(sparse_pattern_b(i) for i in range(BS))),
         "extents": 2, "depth": 0, "holes": [[BS, 2 * BS]]},
        {"path": "/NOTE4K.TXT", "type": "file", "inode": INO_NOTE,
         "size": len(note_data), "sha256": sha256(note_data),
         "extents": 1, "depth": 0},
        {"path": "/LINK.TXT", "type": "symlink", "inode": INO_LINK,
         "size": len(LINK_TARGET), "target": LINK_TARGET.decode()},
    ]
    manifest = {
        "kind": "sutureos-ext4-testimage",
        "version": 1,
        "image": {
            "path": "build/ext4.img",
            "size": TOTAL_BLOCKS * BS,
            "sha256": None,          # filled in after the image bytes are final
            "block_size": BS,
            "blocks": TOTAL_BLOCKS,
            "inode_size": INODE_SIZE,
            "inodes": INODES,
            "inodes_per_group": INODES_PER_GROUP,
            "blocks_per_group": BLOCKS_PER_GROUP,
            "groups": 1,
            "first_data_block": FIRST_DATA_BLOCK,
            "rev_level": 1,
            "features": {
                "compat": 0,
                "incompat": INCOMPAT_FILETYPE | INCOMPAT_EXTENTS,
                "ro_compat": RO_COMPAT_SPARSE_SUPER,
                "names": ["filetype", "extents", "sparse_super"],
            },
            "free_blocks": free_blocks,
            "free_inodes": free_inodes,
            "used_dirs": used_dirs,
            "layout": {
                "superblock_block": BLK_SB,
                "gdt_block": BLK_GDT,
                "block_bitmap": BLK_BLOCK_BITMAP,
                "inode_bitmap": BLK_INODE_BITMAP,
                "inode_table": BLK_INODE_TABLE,
                "inode_table_blocks": INODE_TABLE_BLOCKS,
            },
            "root_inode": 2,
        },
        "dirs": {
            "/": [n.decode() for n, _i, _t in root_entries],
            "/SUBDIR": [n.decode() for n, _i, _t in sub_entries],
        },
        "inode_links": {
            "/": root_nlink,
            "/SUBDIR": sub_nlink,
        },
        "files": files,
    }
    data = bytes(img)
    manifest["image"]["sha256"] = sha256(data)
    return data, manifest


def tree_bytes_for(exts):
    """i_block (60 bytes) for an extent list that fits in the inode."""
    assert len(exts) <= 4
    root = bytearray(extent_header(len(exts), 4, 0))
    for e in exts:
        root += extent_entry(*e)
    root += b"\x00" * (60 - len(root))
    return bytes(root)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


# ===========================================================================
# PART 2 - the independent reader used by --verify
#
# Nothing below calls into PART 1. Every value is re-derived from the image
# bytes: superblock fields are read at their documented offsets, the extent
# trees are walked from i_block, directories are parsed from their data blocks.
# ===========================================================================

class ImageReader:
    def __init__(self, image_bytes):
        self.img = image_bytes

    # -- primitives ---------------------------------------------------------
    def u8(self, off):
        return self.img[off]

    def u16(self, off):
        return struct.unpack_from("<H", self.img, off)[0]

    def u32(self, off):
        return struct.unpack_from("<I", self.img, off)[0]

    def block(self, blk):
        return self.img[blk * self.block_size:(blk + 1) * self.block_size]

    # -- superblock ---------------------------------------------------------
    def read_superblock(self):
        base = 1024
        sb = {
            "inodes_count": self.u32(base + 0),
            "blocks_count": self.u32(base + 4),
            "free_blocks_count": self.u32(base + 12),
            "free_inodes_count": self.u32(base + 16),
            "first_data_block": self.u32(base + 20),
            "log_block_size": self.u32(base + 24),
            "blocks_per_group": self.u32(base + 32),
            "inodes_per_group": self.u32(base + 40),
            "magic": self.u16(base + 56),
            "state": self.u16(base + 58),
            "rev_level": self.u32(base + 76),
            "first_ino": self.u32(base + 84),
            "inode_size": self.u16(base + 88),
            "feature_compat": self.u32(base + 92),
            "feature_incompat": self.u32(base + 96),
            "feature_ro_compat": self.u32(base + 100),
            "uuid": self.img[base + 104:base + 120].hex(),
            "volume_name": self.img[base + 120:base + 136].split(b"\x00")[0].decode("latin1"),
            "desc_size": self.u16(base + 254),
        }
        self.sb = sb
        self.block_size = 1024 << sb["log_block_size"]
        return sb

    def read_group_desc(self, group):
        per_block = self.block_size // 32
        blk = self.first_data_block() + 1 + group // per_block
        off = (group % per_block) * 32
        raw = self.block(blk)
        return {
            "block_bitmap": struct.unpack_from("<I", raw, off + 0)[0],
            "inode_bitmap": struct.unpack_from("<I", raw, off + 4)[0],
            "inode_table": struct.unpack_from("<I", raw, off + 8)[0],
            "free_blocks": struct.unpack_from("<H", raw, off + 12)[0],
            "free_inodes": struct.unpack_from("<H", raw, off + 14)[0],
            "used_dirs": struct.unpack_from("<H", raw, off + 16)[0],
        }

    def first_data_block(self):
        return self.sb["first_data_block"]

    def group_count(self):
        blocks = self.sb["blocks_count"]
        bpg = self.sb["blocks_per_group"]
        ipg = self.sb["inodes_per_group"]
        g1 = (blocks + bpg - 1) // bpg
        g2 = (self.sb["inodes_count"] + ipg - 1) // ipg
        return max(g1, g2)

    # -- inodes -------------------------------------------------------------
    def read_inode(self, ino):
        isz = self.sb["inode_size"]
        ipg = self.sb["inodes_per_group"]
        group = (ino - 1) // ipg
        index = (ino - 1) % ipg
        gd = self.read_group_desc(group)
        off = gd["inode_table"] * self.block_size + index * isz
        raw = self.img[off:off + isz]
        if len(raw) != isz:
            raise ValueError("inode %d is outside the image" % ino)
        return {
            "ino": ino,
            "mode": struct.unpack_from("<H", raw, 0)[0],
            "size": struct.unpack_from("<I", raw, 4)[0]
                    | (struct.unpack_from("<I", raw, 108)[0] << 32),
            "links": struct.unpack_from("<H", raw, 26)[0],
            "blocks_512": struct.unpack_from("<I", raw, 28)[0],
            "flags": struct.unpack_from("<I", raw, 32)[0],
            "i_block": raw[40:100],
            "extra_isize": struct.unpack_from("<H", raw, 128)[0],
        }

    def inode_type(self, ino_blob):
        mode = ino_blob["mode"]
        if mode & 0o170000 == 0o040000:
            return "dir"
        if mode & 0o170000 == 0o100000:
            return "file"
        if mode & 0o170000 == 0o120000:
            return "symlink"
        return "other"

    # -- extent trees -------------------------------------------------------
    def extents_of(self, inode_blob):
        """Return (extent_list, depth, leaf_count). Recursive walk, bounded."""
        root = inode_blob["i_block"]
        out = []
        stats = {"leaves": 0}

        def walk(buf, depth_expected):
            magic, entries, max_entries, depth = struct.unpack_from("<HHHH", buf, 0)
            if magic != EXTENT_MAGIC:
                raise ValueError("extent header magic 0x%04x != 0xF30A" % magic)
            if entries > max_entries:
                raise ValueError("extent header entries %d > max %d" % (entries, max_entries))
            if depth_expected is not None and depth != depth_expected:
                raise ValueError("extent depth %d, expected %d" % (depth, depth_expected))
            if depth > 4:
                raise ValueError("extent depth %d > 4" % depth)
            body = buf[12:]
            if depth == 0:
                stats["leaves"] += 0
                for i in range(entries):
                    ee_block, ee_len, hi, lo = struct.unpack_from("<IHHI", body, i * 12)
                    out.append((ee_block, ee_len, (hi << 32) | lo))
                return depth
            for i in range(entries):
                ei_block, leaf_lo, leaf_hi, _unused = struct.unpack_from(
                    "<IIHH", body, i * 12)
                leaf = (leaf_hi << 32) | leaf_lo
                child = self.block(leaf)
                walk(child, depth - 1)
                stats["leaves"] += 1
            return depth

        depth = walk(root, None)
        out.sort()
        return out, depth, stats["leaves"]

    def map_logical(self, extents, logical):
        for ee_block, ee_len, start in extents:
            length = ee_len & 0x7FFF
            unwritten = ee_len > 32768
            if ee_block <= logical < ee_block + length:
                if unwritten:
                    return None
                return start + (logical - ee_block)
        return None

    def read_inode_data(self, inode_blob):
        """Read the whole file content; holes are zero-filled."""
        size = inode_blob["size"]
        if size == 0:
            return b""
        if inode_blob["flags"] & EXTENTS_FL == 0:
            raise ValueError("inode %d has no extents flag" % inode_blob["ino"])
        extents, _depth, _leaves = self.extents_of(inode_blob)
        out = bytearray()
        nblocks = (size + self.block_size - 1) // self.block_size
        for logical in range(nblocks):
            phys = self.map_logical(extents, logical)
            if phys is None:
                out += bytes(self.block_size)
            else:
                out += self.block(phys)
        return bytes(out[:size])

    # -- directories --------------------------------------------------------
    def read_dir(self, inode_blob):
        data = self.read_inode_data(inode_blob)
        entries = []
        pos = 0
        while pos + 8 <= len(data):
            ino, rec_len, name_len, ftype = struct.unpack_from("<IHBB", data, pos)
            if rec_len < 8 or rec_len % 4 != 0:
                raise ValueError("bad rec_len %d at %d" % (rec_len, pos))
            if pos + rec_len > len(data):
                raise ValueError("rec_len %d runs past the directory" % rec_len)
            if name_len > rec_len - 8:
                raise ValueError("name_len %d does not fit rec_len %d" % (name_len, rec_len))
            name = data[pos + 8:pos + 8 + name_len]
            if ino:
                entries.append((name.decode("latin1"), ino, ftype))
            pos += rec_len
        return entries

    # -- bitmaps ------------------------------------------------------------
    def bitmap_counts(self):
        gd = self.read_group_desc(0)
        bb = self.block(gd["block_bitmap"])
        ib = self.block(gd["inode_bitmap"])
        total_blocks = self.sb["blocks_count"]
        used_blocks = 0
        for b in range(total_blocks):
            if bb[b // 8] & (1 << (b % 8)):
                used_blocks += 1
        used_inodes = 0
        for i in range(self.sb["inodes_count"]):
            if ib[i // 8] & (1 << (i % 8)):
                used_inodes += 1
        return {
            "used_blocks": used_blocks,
            "free_blocks": total_blocks - used_blocks,
            "used_inodes": used_inodes,
            "free_inodes": self.sb["inodes_count"] - used_inodes,
            "block_bitmap": bb,
            "inode_bitmap": ib,
        }


def verify_image(image_bytes, manifest):
    """Independently re-parse the image and compare it with the manifest."""
    checks = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond), detail))

    r = ImageReader(image_bytes)
    sb = r.read_superblock()

    # ---- 1. superblock geometry -------------------------------------------
    check("superblock magic is 0xEF53", sb["magic"] == SUPER_MAGIC,
          "0x%04x" % sb["magic"])
    check("block size is 4096", r.block_size == BS, str(r.block_size))
    check("inode size is 256", sb["inode_size"] == INODE_SIZE, str(sb["inode_size"]))
    check("blocks_count is 4096 (16 MiB)", sb["blocks_count"] == TOTAL_BLOCKS,
          str(sb["blocks_count"]))
    check("inodes_count is 64", sb["inodes_count"] == INODES, str(sb["inodes_count"]))
    check("group count is 1", r.group_count() == 1, str(r.group_count()))
    check("first_data_block is 0 (4 KiB blocks)", sb["first_data_block"] == 0,
          str(sb["first_data_block"]))
    check("revision is 1 (dynamic)", sb["rev_level"] == 1, str(sb["rev_level"]))
    check("first_ino is 11", sb["first_ino"] == 11, str(sb["first_ino"]))
    check("desc_size is 0 or 32 (no 64bit)",
          sb["desc_size"] in (0, 32), str(sb["desc_size"]))
    check("filesystem is marked clean", sb["state"] & 1 == 1, "state=0x%x" % sb["state"])
    check("volume name is sutureos-e4", sb["volume_name"] == "sutureos-e4",
          sb["volume_name"])

    # ---- 2. features ------------------------------------------------------
    want_incompat = INCOMPAT_FILETYPE | INCOMPAT_EXTENTS
    check("feature_compat is 0 (no journal/dir_index)", sb["feature_compat"] == 0,
          "0x%x" % sb["feature_compat"])
    check("feature_incompat is exactly filetype|extents",
          sb["feature_incompat"] == want_incompat, "0x%x" % sb["feature_incompat"])
    check("filetype feature is on",
          sb["feature_incompat"] & INCOMPAT_FILETYPE != 0)
    check("extents feature is on", sb["feature_incompat"] & INCOMPAT_EXTENTS != 0)
    check("feature_ro_compat is exactly sparse_super",
          sb["feature_ro_compat"] == RO_COMPAT_SPARSE_SUPER,
          "0x%x" % sb["feature_ro_compat"])
    for name, bit in (("metadata_csum", 0x0400), ("bigalloc", 0x0200),
                      ("inline_data", 0x8000), ("64bit", 0x0080),
                      ("mmp", 0x0100), ("dir_index", 0x0020)):
        if name == "dir_index":
            absent = sb["feature_compat"] & bit == 0
        elif name in ("metadata_csum", "bigalloc"):
            absent = sb["feature_ro_compat"] & bit == 0
        else:
            absent = sb["feature_incompat"] & bit == 0
        check("feature %s is NOT enabled" % name, absent)

    # ---- 3. group descriptor + bitmaps ------------------------------------
    gd = r.read_group_desc(0)
    check("group descriptor block bitmap follows the GDT",
          gd["block_bitmap"] == BLK_BLOCK_BITMAP, str(gd["block_bitmap"]))
    check("group descriptor inode bitmap",
          gd["inode_bitmap"] == BLK_INODE_BITMAP, str(gd["inode_bitmap"]))
    check("group descriptor inode table",
          gd["inode_table"] == BLK_INODE_TABLE, str(gd["inode_table"]))
    counts = r.bitmap_counts()
    check("superblock free_blocks matches the block bitmap",
          sb["free_blocks_count"] == counts["free_blocks"],
          "sb=%d bitmap=%d" % (sb["free_blocks_count"], counts["free_blocks"]))
    check("group descriptor free_blocks matches the block bitmap",
          gd["free_blocks"] == counts["free_blocks"],
          "gd=%d bitmap=%d" % (gd["free_blocks"], counts["free_blocks"]))
    check("superblock free_inodes matches the inode bitmap",
          sb["free_inodes_count"] == counts["free_inodes"],
          "sb=%d bitmap=%d" % (sb["free_inodes_count"], counts["free_inodes"]))
    check("group descriptor free_inodes matches the inode bitmap",
          gd["free_inodes"] == counts["free_inodes"],
          "gd=%d bitmap=%d" % (gd["free_inodes"], counts["free_inodes"]))
    check("group descriptor used_dirs is 2", gd["used_dirs"] == 2, str(gd["used_dirs"]))
    # every data block referenced by the inodes must be marked used
    referenced = set()
    for f in manifest["files"]:
        if f["type"] == "symlink":
            continue
        ino = r.read_inode(f["inode"])
        exts, _d, _l = r.extents_of(ino)
        for _logical, length, start in exts:
            for k in range(length & 0x7FFF):
                referenced.add(start + k)
    check("no referenced data block is marked free in the bitmap",
          all(counts["block_bitmap"][b // 8] & (1 << (b % 8)) for b in referenced),
          "%d blocks referenced" % len(referenced))
    check("no referenced data block is out of range",
          all(0 < b < sb["blocks_count"] for b in referenced))

    # ---- 4. manifest files: path resolution, content, sha256 --------------
    root = r.read_inode(2)
    check("root inode has the extents flag", root["flags"] & EXTENTS_FL != 0)
    check("root inode mode is a directory", r.inode_type(root) == "dir")

    def resolve(path):
        parts = [p for p in path.split("/") if p]
        ino = root
        for part in parts:
            if r.inode_type(ino) != "dir":
                return None
            found = None
            for name, child, _ft in r.read_dir(ino):
                if name == part:
                    found = child
                    break
            if found is None:
                return None
            ino = r.read_inode(found)
        return ino

    for f in manifest["files"]:
        blob = resolve(f["path"])
        check("%s resolves through the directory tree" % f["path"], blob is not None)
        if blob is None:
            continue
        check("%s has the manifest type" % f["path"],
              r.inode_type(blob) == f["type"],
              "%s != %s" % (r.inode_type(blob), f["type"]))
        check("%s size matches the manifest" % f["path"],
              blob["size"] == f["size"], "%d != %d" % (blob["size"], f["size"]))
        if f["type"] == "file":
            content = r.read_inode_data(blob)
            check("%s content sha256 matches the manifest" % f["path"],
                  sha256(content) == f["sha256"],
                  sha256(content) if sha256(content) != f["sha256"] else "")
            exts, depth, leaves = r.extents_of(blob)
            check("%s extent count matches the manifest" % f["path"],
                  len(exts) == f["extents"], "%d != %d" % (len(exts), f["extents"]))
            check("%s extent depth matches the manifest" % f["path"],
                  depth == f["depth"], "%d != %d" % (depth, f["depth"]))
            if f["extents"]:
                nblocks = (f["size"] + BS - 1) // BS
                covered = {}
                for logical, length, start in exts:
                    for k in range(length & 0x7FFF):
                        covered[logical + k] = start + k
                missing = [b for b in range(nblocks) if b not in covered]
                if f.get("holes"):
                    holes = set()
                    for a, b in f["holes"]:
                        for lb in range(a // BS, (b + BS - 1) // BS):
                            holes.add(lb)
                    missing = [b for b in missing if b not in holes]
                check("%s has no unexpected hole (all logical blocks mapped)" % f["path"],
                      not missing, "missing=%r" % (missing[:8],))
                starts = list(covered.values())
                check("%s has no duplicate physical blocks" % f["path"],
                      len(starts) == len(set(starts)))
        elif f["type"] == "symlink":
            target = blob["i_block"][:blob["size"]].split(b"\x00")[0]
            check("%s symlink target matches the manifest" % f["path"],
                  target == f["target"].encode(), target.decode("latin1"))

    # ---- 5. BIG.BIN / FRAG.BIN extent-tree shape --------------------------
    for path in ("/BIG.BIN", "/SUBDIR/FRAG.BIN"):
        f = [x for x in manifest["files"] if x["path"] == path][0]
        blob = resolve(path)
        exts, depth, leaves = r.extents_of(blob)
        check("%s uses a depth-1 extent tree" % path, depth == 1, str(depth))
        check("%s has two leaf blocks" % path, leaves == 2, str(leaves))
        check("%s has one extent per 4 KiB block (>340 extents => multi-leaf)" % path,
              len(exts) == 384, str(len(exts)))
        check("%s extents are in ascending logical order" % path,
              all(exts[i][0] < exts[i + 1][0] for i in range(len(exts) - 1)))
        check("%s extent lengths are all 1 block" % path,
              all((e[1] & 0x7FFF) == 1 for e in exts))

    # ---- 6. directories ---------------------------------------------------
    for dpath, want in manifest["dirs"].items():
        ino = root if dpath == "/" else resolve(dpath)
        got = sorted(n for n, _i, _t in r.read_dir(ino))
        check("%s directory listing matches" % dpath, got == sorted(want),
              "got=%r" % (got,))
        check("%s contains '.' and '..'" % dpath,
              "." in got and ".." in got, "got=%r" % (got,))
    # filetype byte must agree with the target inode type
    ft_map = {"file": FT_REG, "dir": FT_DIR, "symlink": FT_SYMLINK}
    bad_ft = []
    for name, ino, ftype in r.read_dir(root):
        if name in (".", ".."):
            continue
        child = r.read_inode(ino)
        if ft_map.get(r.inode_type(child)) != ftype:
            bad_ft.append(name)
    check("root dirent file_type bytes match the inode modes", not bad_ft,
          "bad=%r" % (bad_ft,))

    # ---- 7. link counts ---------------------------------------------------
    subdirs = [n for n, i, t in r.read_dir(root)
               if t == FT_DIR and n not in (".", "..")]
    check("root inode link count is 2 + subdirectories",
          root["links"] == 2 + len(subdirs), "%d vs %d" % (root["links"], 2 + len(subdirs)))
    sub = r.read_inode([i for n, i, t in r.read_dir(root) if n == "SUBDIR"][0])
    check("SUBDIR inode link count is 2", sub["links"] == 2, str(sub["links"]))
    for f in manifest["files"]:
        if f["type"] == "file":
            blob = resolve(f["path"])
            check("%s link count is 1" % f["path"], blob["links"] == 1,
                  str(blob["links"]))

    # ---- 8. image hash (read-only proof for the host verifier) ------------
    check("image sha256 matches the manifest (image was not modified)",
          sha256(image_bytes) == manifest["image"]["sha256"])

    return checks


# ===========================================================================
# PART 3 - CLI
# ===========================================================================

def report(checks, title):
    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        if not ok:
            print("  [FAIL] %s%s" % (name, (" (%s)" % detail) if detail else ""))
    print("[%s] checks=%d ok=%d fail=%d"
          % (title, len(checks), len(checks) - len(failed), len(failed)))
    print("[%s] RESULT: %s" % (title, "PASS" if not failed else "FAIL"))
    return 0 if not failed else 1


# ===========================================================================
# PART 3 - negative fixtures (superblock feature-word mutations)
#
# These prove the driver REFUSES what it does not implement instead of guessing:
# each variant flips one feature bit, and mounting it must fail with that exact
# bit named (or, for a ro_compat bit, mount with an explicit note - which is what
# the read-only-compatible contract allows). They are intentionally NOT in the
# manifest (their sha256 differs) and the acceptance run only uses ext4.img.
# ===========================================================================
NEGATIVES = [
    ("neg64bit", "incompat", 0x0080, "must be refused: 64bit"),
    ("neginline", "incompat", 0x8000, "must be refused: inline_data"),
    ("negbigalloc", "incompat", 0x0200, "must be refused: bigalloc"),
    ("negmeta_bg", "incompat", 0x0010, "must be refused: meta_bg"),
    ("negmmp", "incompat", 0x0100, "must be refused: mmp"),
    ("negdirindex", "compat", 0x0020, "must be refused: dir_index"),
    ("negjournal", "compat", 0x0004, "may mount, with a has_journal note"),
    ("negcsum", "ro_compat", 0x0400, "may mount, with a metadata_csum note"),
]
WORD_OFFSET = {"compat": 92, "incompat": 96, "ro_compat": 100}


def write_negatives(image, out_dir):
    """Write one mutated copy per NEGATIVES entry; returns [(path, description)]."""
    made = []
    for suffix, word, bit, expect in NEGATIVES:
        img = bytearray(image)
        off = 1024 + WORD_OFFSET[word]
        cur = struct.unpack_from("<I", img, off)[0]
        struct.pack_into("<I", img, off, cur | bit)
        path = os.path.join(out_dir, "ext4_%s.img" % suffix)
        with open(path, "wb") as fh:
            fh.write(img)
        made.append((path, "%s %s 0x%04x: %s" % (suffix, word, bit, expect)))
    return made


def main():
    ap = argparse.ArgumentParser(description="build/verify the SutureOS ext4 test image")
    ap.add_argument("--verify", action="store_true",
                    help="only re-parse the existing build/ext4.img + manifest")
    ap.add_argument("--negatives", action="store_true",
                    help="also write the mutated fixtures that prove the driver "
                         "refuses unsupported features")
    ap.add_argument("--out", default=IMG_PATH)
    ap.add_argument("--manifest", default=MANIFEST_PATH)
    args = ap.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)

    if args.verify:
        with open(args.out, "rb") as fh:
            image = fh.read()
        with open(args.manifest, encoding="utf-8") as fh:
            manifest = json.load(fh)
        print("[mkfs_ext4] verifying %s (%d bytes)" % (args.out, len(image)))
        return report(verify_image(image, manifest), "mkfs_ext4")

    image, manifest = build_image()
    with open(args.out, "wb") as fh:
        fh.write(image)
    with open(args.manifest, "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=2, sort_keys=True)
        fh.write("\n")
    img_info = manifest["image"]
    print("ext4 image  : %s (%d bytes)" % (args.out, len(image)))
    print("  geometry  : block=%d inode=%d blocks=%d inodes=%d groups=%d"
          % (BS, INODE_SIZE, TOTAL_BLOCKS, INODES, 1))
    print("  features  : incompat=0x%04x (filetype|extents) ro_compat=0x%04x "
          "(sparse_super) compat=0x0000"
          % (INCOMPAT_FILETYPE | INCOMPAT_EXTENTS, RO_COMPAT_SPARSE_SUPER))
    print("  free      : blocks=%d inodes=%d" % (img_info["free_blocks"],
                                                 img_info["free_inodes"]))
    for f in manifest["files"]:
        extra = ""
        if f["type"] == "file" and f["extents"]:
            extra = " extents=%d depth=%d" % (f["extents"], f["depth"])
        elif f["type"] == "symlink":
            extra = " -> %s" % f["target"]
        print("  %-18s %-8s %8d bytes%s"
              % (f["path"], f["type"], f["size"], extra))
    print("manifest    : %s" % args.manifest)
    r = ImageReader(image)
    r.read_superblock()
    print("  image sha256: %s" % img_info["sha256"])
    print("  superblock is readable at byte offset 1024, magic=0x%04x"
          % r.sb["magic"])
    if args.negatives:
        for path, desc in write_negatives(image, OUT_DIR):
            print("  negative   : %s (%s)" % (os.path.basename(path), desc))
    return report(verify_image(image, manifest), "mkfs_ext4")


if __name__ == "__main__":
    sys.exit(main())
