#!/usr/bin/env python3
"""SutureOS: VimtuFS2 test-image generator (host side, pure standard library).

WHY THIS FILE EXISTS
  VimtuFS2 is the on-disk filesystem of VimtuOS.  SutureOS gained a read-only
  driver for it (kernel/fs/vimtufs2.c); this generator produces the volume the
  driver is proven against, plus a manifest the host verifier can hash-check.

WHERE THE FORMAT COMES FROM (licence + provenance)
  Format spec + reference implementation: VimtuOS
      kernel/vfs64.h  (file-header section "磁盘布局" / inode table)
      kernel/vfs64.cpp (VFS_O_* / VFS_I_* offsets, sb_verify, vfs64_format)
  VimtuOS is GPL-3.0.  SutureOS is GPL-3.0 as well, so the format may be
  implemented freely - but this generator is **written from the spec**, it is
  not a copy of vfs64.cpp: no VimtuOS code was transcribed.  The values below
  were re-derived from the spec and then cross-checked against the C++
  static_asserts (vfs64.cpp:139-165) and against vfs64_format (vfs64.cpp:3486-3623).

DISK LAYOUT (block = 512 B, block numbers relative to the volume start)
  block 0                superblock (magic "VIMTUFS2", 0xAA55 @510, CRC32 @60
                         over [0,60); v5 feature header at [64,88))
  blocks 1 .. 1+bmn-1    free-block bitmap (1 bit per block, 1 = used)
  blocks bp .. bp+ibn-1  inode table (128 B per inode, 4 per block)
  blocks dp .. total-1   data area (file contents + indirect pointer blocks)
    bmn = ceil(total/4096), ibn = ceil(inodes/4), bp = 1+bmn, dp = bp+ibn

INODE (128 B, little endian; offsets are the spec's, see vfs64.h lines 36-64)
  0   u8  type      0 = free / 1 = file / 2 = dir / 3 = symlink
  1   u8  namelen   0..31 (0 only for the root directory)
  2   u16 rsvd      must be 0
  4   u32 size      byte count (directories are always 0: they have no data)
  8   u32 d0..d3    direct blocks (offsets 8/12/16/20)
  24  u32 ind       single indirect block (128 block numbers)
  28  u32 parent    parent inode number (root points at itself = 0)
  32  u32 mtime     packed (year-2000)<<26 | month<<22 | day<<17 | hour<<12 |
                    minute<<6 | second; 0 = unknown
  36  u16 nlink     file = 1; directory = 1 + number of direct subdirectories
  38  u8  kind      cached content kind (0..6)
  39  u8  rsvd2     must be 0
  40  31B name      ASCII, not NUL terminated on disk
  71  u32 dind      double indirect block (v3/v4/v5 only; 0 = none)
  75  u16 uid / 77 gid / 79 mode   (v4/v5 only; must be 0 on v3)
  81  .. reserved   v3: [75,124)  v4: [81,124)  v5: xattr@81, snapf@85,
                    reserved [89,124) - all must be 0 for a feature-free volume
  124 u32 crc32     over [0,124)

BIG2.BIN is deliberately > 132 filesystem blocks so it needs BOTH the single
indirect block and the double indirect block including a second child block.
The data blocks are allocated through a coprime stride, i.e. genuinely
scattered, so a driver that just reads "the next block" instead of following
the pointer table cannot pass.

USAGE
  py -3 scripts/mkfs_vimtufs2_testimg.py                 # build/vimtufs2.img (v5)
  py -3 scripts/mkfs_vimtufs2_testimg.py --version 4     # v4 variant
  py -3 scripts/mkfs_vimtufs2_testimg.py --features 0xf \
        --out build/vimtufs2_badfeat.img                 # negative fixture
  py -3 scripts/mkfs_vimtufs2_testimg.py --verify        # independent re-parse

`--verify` never trusts the builder: it re-reads the bytes just written,
recomputes the geometry, the CRC32s and the block mapping, and compares the
file contents against the manifest's SHA-256.
"""
import argparse
import hashlib
import json
import math
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUT = ROOT / "build" / "vimtufs2.img"
DEFAULT_MANIFEST = ROOT / "build" / "vimtufs2_manifest.json"

# ---- geometry / format constants (VimtuOS vfs64.h:166-204) -------------------
SECTOR_BYTES = 512
BLOCK_BYTES = 512
INODE_BYTES = 128
INODES_PER_BLOCK = 4
NAME_MAX = 31
BITMAP_BLOCK_BITS = BLOCK_BYTES * 8          # 4096 block bits per bitmap block
DIRECT_BLOCKS = 4
INDIRECT_PTRS = 128
DIND_CHILDREN = 128
L2_FIRST_BLOCK = DIRECT_BLOCKS + INDIRECT_PTRS          # 132
MAX_MAP_BLOCKS = DIRECT_BLOCKS + INDIRECT_PTRS + DIND_CHILDREN * INDIRECT_PTRS
MAX_FILE_BLOCKS = 16384                                # 8 MiB exactly
MAX_INODES = 2048
MIN_BLOCKS = 32

VERSION_V3, VERSION_V4, VERSION_V5 = 3, 4, 5

# superblock offsets (vfs64.cpp:39-65)
SB_MAGIC = 0
SB_VERSION = 8
SB_SECTOR = 12
SB_BLOCK = 16
SB_TOTAL = 20
SB_ROOT = 24
SB_BITMAP = 28
SB_BITMAPN = 32
SB_INODE = 36
SB_INODEN = 40
SB_INOSZ = 44
SB_DATA = 48
SB_DATAN = 52
SB_FLAGS = 56
SB_CRC = 60
SB_CRC_LEN = 60
SB_FEATBITS = 64
SB_JX_START = 68
SB_JX_NBLK = 72
SB_QUOTA = 76
SB_SNAP = 80
SB_FEATCRC = 84
SB_FEAT_CRC_LEN = 20
SB_SIG = 510

# feature bits (vfs64.h:220-227)
FEAT_JOURNAL, FEAT_QUOTA, FEAT_ACL, FEAT_SNAPSHOT = 1, 2, 4, 8
FEAT_ALL = FEAT_JOURNAL | FEAT_QUOTA | FEAT_ACL | FEAT_SNAPSHOT
FEAT_NAMES = ((FEAT_JOURNAL, "journal"), (FEAT_QUOTA, "quota"),
              (FEAT_ACL, "acl"), (FEAT_SNAPSHOT, "snapshot"))

# inode offsets (vfs64.cpp:69-97)
I_TYPE, I_NAMELEN, I_RSVD, I_SIZE = 0, 1, 2, 4
I_D0, I_IND, I_PARENT, I_MTIME, I_NLINK = 8, 24, 28, 32, 36
I_KIND, I_RSVD2, I_NAME, I_DIND = 38, 39, 40, 71
I_UID, I_GID, I_MODE = 75, 77, 79
I_XATTR, I_SNAPF = 81, 85
I_CRC = 124

# inode types / kinds
T_FREE, T_FILE, T_DIR, T_LINK = 0, 1, 2, 3
K_NONE, K_FILE, K_DIR, K_VAP, K_ELF, K_TEXT, K_BIN = 0, 1, 2, 3, 4, 5, 6

# mode bits (vfs64.h:293-308)
S_IFMT = 0o170000
S_IFREG = 0o100000
S_IFDIR = 0o040000
S_IFLNK = 0o120000
DIR_MODE = S_IFDIR | 0o755
FILE_MODE = S_IFREG | 0o644

MAGIC = b"VIMTUFS2"


def le16(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


def le32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def put16(buf, off, val):
    struct.pack_into("<H", buf, off, val & 0xFFFF)


def put32(buf, off, val):
    struct.pack_into("<I", buf, off, val & 0xFFFFFFFF)


def crc32(data):
    """Standard CRC-32 (reflected 0xEDB88320, init/xorout 0xFFFFFFFF).

    Same function as VimtuOS crc32_64 (vfs64.cpp:206-213); zlib implements it.
    Known answer: crc32(b"123456789") == 0xCBF43926.
    """
    return zlib.crc32(data) & 0xFFFFFFFF


def pack_mtime(year, month, day, hour, minute, second):
    if not (2000 <= year <= 2063 and 1 <= month <= 12 and 1 <= day <= 31 and
            hour <= 23 and minute <= 59 and second <= 59):
        raise ValueError("mtime out of range")
    return (((year - 2000) & 0x3F) << 26 | (month & 0xF) << 22 |
            (day & 0x1F) << 17 | (hour & 0x1F) << 12 |
            (minute & 0x3F) << 6 | (second & 0x3F))


def geometry(total_blocks):
    """Volume geometry exactly as vfs64_format computes it (vfs64.cpp:3486-3502)."""
    if total_blocks < MIN_BLOCKS:
        raise ValueError("volume too small: %d blocks < %d" % (total_blocks, MIN_BLOCKS))
    bitmap_blocks = (total_blocks + BITMAP_BLOCK_BITS - 1) // BITMAP_BLOCK_BITS
    inodes = total_blocks // 16
    if inodes < 16:
        inodes = 16
    if inodes > MAX_INODES:
        inodes = MAX_INODES
    inode_blocks = (inodes + INODES_PER_BLOCK - 1) // INODES_PER_BLOCK
    bitmap_start = 1
    inode_start = bitmap_start + bitmap_blocks
    data_start = inode_start + inode_blocks
    data_blocks = total_blocks - data_start
    if data_blocks <= 0:
        raise ValueError("no data blocks")
    return {
        "blocks": total_blocks,
        "bitmap_start": bitmap_start,
        "bitmap_blocks": bitmap_blocks,
        "inode_start": inode_start,
        "inode_blocks": inode_blocks,
        "inodes": inodes,
        "data_start": data_start,
        "data_blocks": data_blocks,
    }


class Entry:
    """One inode of the fixture: kind of node, name, parent and payload."""

    def __init__(self, index, parent, typ, name, payload=b"", kind=K_FILE):
        self.index = index
        self.parent = parent
        self.type = typ
        self.name = name
        self.payload = payload
        self.kind = kind
        self.blocks = []          # data blocks, filesystem block order
        self.ind = 0
        self.dind = 0
        self.children = []        # second-level indirect child blocks

    @property
    def namelen(self):
        return len(self.name.encode("ascii"))

    @property
    def size(self):
        return len(self.payload)

    @property
    def fs_blocks(self):
        return (len(self.payload) + BLOCK_BYTES - 1) // BLOCK_BYTES


def big2_payload(nbytes):
    """Deterministic, position-dependent payload: a misordered block shows up."""
    out = bytearray(nbytes)
    for i in range(nbytes):
        out[i] = (i * 31 + (i >> 8) * 7 + 0x5A) & 0xFF
    return bytes(out)


def scattered_order(data_blocks, count):
    """Deterministic permutation of [0, data_blocks) so allocations are scattered.

    stride must be coprime with data_blocks; 7919 (prime) is tried first so the
    image layout is identical for a given volume size.
    """
    stride = 7919
    while math.gcd(stride, data_blocks) != 1:
        stride += 2
    if data_blocks <= count:
        return list(range(count))
    seen = set()
    out = []
    k = 0
    while len(out) < count:
        blk = (k * stride) % data_blocks
        k += 1
        if blk in seen:
            continue
        seen.add(blk)
        out.append(blk)
    return out


def build_image(total_blocks, version, feature_bits=0):
    """Build the whole volume in memory; returns (image_bytes, plan)."""
    if version not in (VERSION_V3, VERSION_V4, VERSION_V5):
        raise ValueError("only v3/v4/v5 are generated (got %r)" % (version,))
    g = geometry(total_blocks)
    image = bytearray(total_blocks * BLOCK_BYTES)
    mtime = pack_mtime(2026, 11, 20, 17, 30, 0)

    long_name = "ABCDEFGHIJKLMNOPQRSTUVWXYZ01234"          # exactly 31 bytes
    if len(long_name) != NAME_MAX:
        raise AssertionError("the 31-byte name fixture changed length")

    entries = [
        Entry(0, 0, T_DIR, "", kind=K_DIR),
        Entry(1, 0, T_FILE, "HELLO.TXT", b"Hello from VimtuFS2!\n", K_TEXT),
        # 300 blocks: 4 direct + 128 single-indirect + 168 double-indirect
        # (two child blocks: 128 + 40).
        Entry(2, 0, T_FILE, "BIG2.BIN", big2_payload(300 * BLOCK_BYTES), K_BIN),
        Entry(3, 0, T_DIR, "SUBDIR", kind=K_DIR),
        Entry(4, 3, T_FILE, "INNER.TXT", b"This file lives in SUBDIR.\n", K_TEXT),
        Entry(5, 0, T_FILE, "EMPTY", b"", K_FILE),
        Entry(6, 0, T_FILE, long_name, b"31-byte name\n", K_TEXT),
    ]
    for e in entries:
        if e.namelen > NAME_MAX:
            raise AssertionError("name too long: %r" % (e.name,))
        if e.fs_blocks > MAX_FILE_BLOCKS:
            raise AssertionError("file too big: %r" % (e.name,))

    # ---- allocate every block through one scattered permutation -------------
    need = sum(e.fs_blocks for e in entries if e.type == T_FILE)
    need += 4               # BIG2: ind + dind + 2 children (checked below)
    perm = scattered_order(g["data_blocks"], need)
    cursor = [0]

    def take():
        blk = g["data_start"] + perm[cursor[0]]
        cursor[0] += 1
        return blk

    for e in entries:
        if e.type != T_FILE:
            continue
        e.blocks = [take() for _ in range(e.fs_blocks)]

    for e in entries:
        n = len(e.blocks)
        if n > DIRECT_BLOCKS:
            e.ind = take()
        if n > L2_FIRST_BLOCK:
            e.dind = take()
            nchild = (n - L2_FIRST_BLOCK + INDIRECT_PTRS - 1) // INDIRECT_PTRS
            e.children = [take() for _ in range(nchild)]
    if cursor[0] != need:
        raise AssertionError("allocation plan mismatch: used %d of %d" %
                             (cursor[0], need))

    used = set()

    def mark(blk):
        if blk in used:
            raise AssertionError("block %d allocated twice" % blk)
        if not (g["data_start"] <= blk < total_blocks):
            raise AssertionError("block %d outside the data area" % blk)
        used.add(blk)

    # ---- write file data + pointer blocks -----------------------------------
    for e in entries:
        if e.type != T_FILE:
            continue
        for blk in e.blocks:
            mark(blk)
        for i, blk in enumerate(e.blocks):
            chunk = e.payload[i * BLOCK_BYTES:(i + 1) * BLOCK_BYTES]
            image[blk * BLOCK_BYTES:blk * BLOCK_BYTES + len(chunk)] = chunk
        if e.ind:
            mark(e.ind)
            table = bytearray(BLOCK_BYTES)
            for i in range(INDIRECT_PTRS):
                fs_blk = DIRECT_BLOCKS + i
                if fs_blk < len(e.blocks):
                    put32(table, 4 * i, e.blocks[fs_blk])
            image[e.ind * BLOCK_BYTES:(e.ind + 1) * BLOCK_BYTES] = table
        if e.dind:
            mark(e.dind)
            table = bytearray(BLOCK_BYTES)
            for i, child in enumerate(e.children):
                mark(child)
                put32(table, 4 * i, child)
                ctable = bytearray(BLOCK_BYTES)
                for j in range(INDIRECT_PTRS):
                    fs_blk = L2_FIRST_BLOCK + i * INDIRECT_PTRS + j
                    if fs_blk < len(e.blocks):
                        put32(ctable, 4 * j, e.blocks[fs_blk])
                image[child * BLOCK_BYTES:(child + 1) * BLOCK_BYTES] = ctable
            image[e.dind * BLOCK_BYTES:(e.dind + 1) * BLOCK_BYTES] = table

    # ---- bitmap: metadata + every allocated block is marked used ----------
    bitmap = bytearray(g["bitmap_blocks"] * BLOCK_BYTES)
    for blk in range(0, g["data_start"]):
        bitmap[blk >> 3] |= 1 << (blk & 7)
    for blk in used:
        bitmap[blk >> 3] |= 1 << (blk & 7)
    # tail bits beyond the volume must stay marked so an allocator never hands
    # them out (vfs64_format does the same, vfs64.cpp:3562-3565)
    for blk in range(total_blocks, g["bitmap_blocks"] * BITMAP_BLOCK_BITS):
        bitmap[blk >> 3] |= 1 << (blk & 7)
    image[g["bitmap_start"] * BLOCK_BYTES:
          (g["bitmap_start"] + g["bitmap_blocks"]) * BLOCK_BYTES] = bitmap

    # ---- inode table -------------------------------------------------------
    # directory nlink = 1 + number of direct subdirectories (vfs64.h:48)
    subdir_count = {}
    for e in entries:
        if e.type == T_DIR and e.parent != e.index:
            subdir_count[e.parent] = subdir_count.get(e.parent, 0) + 1

    inode_bytes = {}
    for e in entries:
        buf = bytearray(INODE_BYTES)
        buf[I_TYPE] = e.type
        buf[I_NAMELEN] = e.namelen
        put16(buf, I_RSVD, 0)
        put32(buf, I_SIZE, e.size if e.type == T_FILE else 0)
        for i in range(DIRECT_BLOCKS):
            put32(buf, I_D0 + 4 * i, e.blocks[i] if i < len(e.blocks) else 0)
        put32(buf, I_IND, e.ind)
        put32(buf, I_PARENT, e.parent)
        put32(buf, I_MTIME, mtime)
        nlink = 1 + subdir_count.get(e.index, 0) if e.type == T_DIR else 1
        put16(buf, I_NLINK, nlink)
        buf[I_KIND] = e.kind
        buf[I_RSVD2] = 0
        name = e.name.encode("ascii")
        buf[I_NAME:I_NAME + len(name)] = name
        # dind exists on v3/v4/v5 alike (batch M put it at 71, the start of what
        # used to be the reserved area); only v2 lacks it.
        put32(buf, I_DIND, e.dind)
        if version >= VERSION_V4:
            put16(buf, I_UID, 0)
            put16(buf, I_GID, 0)
            put16(buf, I_MODE, DIR_MODE if e.type == T_DIR else FILE_MODE)
        if version >= VERSION_V5:
            put32(buf, I_XATTR, 0)
            put32(buf, I_SNAPF, 0)
        put32(buf, I_CRC, crc32(bytes(buf[:I_CRC])))
        inode_bytes[e.index] = bytes(buf)

    # inode 0 is the root directory: no name, parent = itself, one link + subdirs
    root = bytearray(inode_bytes[0])
    root[I_NAMELEN] = 0
    put32(root, I_PARENT, 0)
    put32(root, I_CRC, 0)
    put32(root, I_CRC, crc32(bytes(root[:I_CRC])))
    inode_bytes[0] = bytes(root)

    for idx, raw in inode_bytes.items():
        blk = g["inode_start"] + idx // INODES_PER_BLOCK
        off = (idx % INODES_PER_BLOCK) * INODE_BYTES
        image[blk * BLOCK_BYTES + off:blk * BLOCK_BYTES + off + INODE_BYTES] = raw

    # ---- superblock --------------------------------------------------------
    sb = bytearray(BLOCK_BYTES)
    sb[SB_MAGIC:SB_MAGIC + 8] = MAGIC
    put32(sb, SB_VERSION, version)
    put32(sb, SB_SECTOR, SECTOR_BYTES)
    put32(sb, SB_BLOCK, BLOCK_BYTES)
    put32(sb, SB_TOTAL, total_blocks)
    put32(sb, SB_ROOT, 0)
    put32(sb, SB_BITMAP, g["bitmap_start"])
    put32(sb, SB_BITMAPN, g["bitmap_blocks"])
    put32(sb, SB_INODE, g["inode_start"])
    put32(sb, SB_INODEN, g["inodes"])
    put32(sb, SB_INOSZ, INODE_BYTES)
    put32(sb, SB_DATA, g["data_start"])
    put32(sb, SB_DATAN, g["data_blocks"])
    put32(sb, SB_FLAGS, 0)
    put32(sb, SB_CRC, crc32(bytes(sb[:SB_CRC_LEN])))
    sb[SB_SIG] = 0x55
    sb[SB_SIG + 1] = 0xAA
    if feature_bits:
        if version != VERSION_V5:
            raise ValueError("feature bits are only defined on v5")
        if feature_bits & ~FEAT_ALL:
            raise ValueError("unknown feature bits 0x%x" % feature_bits)
        # Put the journal / quota / snapshot tables inside the data area so the
        # negative fixture is geometrically plausible: the driver has to reject
        # it because of the *feature bits*, not because of a bad block number.
        put32(sb, SB_FEATBITS, feature_bits)
        if feature_bits & FEAT_JOURNAL:
            put32(sb, SB_JX_START, g["data_start"])
            put32(sb, SB_JX_NBLK, 2)
        if feature_bits & FEAT_QUOTA:
            put32(sb, SB_QUOTA, g["data_start"] + 2)
        if feature_bits & FEAT_SNAPSHOT:
            put32(sb, SB_SNAP, g["data_start"] + 3)
        put32(sb, SB_FEATCRC, crc32(bytes(sb[SB_FEATBITS:SB_FEATBITS + SB_FEAT_CRC_LEN])))
    image[0:BLOCK_BYTES] = sb

    free_blocks = g["data_blocks"] - len(used)
    plan = {
        "version": version,
        "feature_bits": feature_bits,
        "geometry": g,
        "mtime": mtime,
        "free_blocks": free_blocks,
        "used_data_blocks": sorted(used),
        "entries": entries,
        "sb_crc32": le32(sb, SB_CRC),
    }
    return bytes(image), plan


# ============================================================================
# Manifest
# ============================================================================

def make_manifest(image, plan, image_path, manifest_path):
    g = plan["geometry"]
    files = []
    for e in plan["entries"]:
        files.append({
            "name": e.name,
            "inode": e.index,
            "parent": e.parent,
            "type": "dir" if e.type == T_DIR else "file",
            "size": e.size if e.type == T_FILE else 0,
            "fs_blocks": e.fs_blocks,
            "data_blocks": e.blocks,
            "ind_block": e.ind,
            "dind_block": e.dind,
            "dind_child_blocks": e.children,
            "sha256": hashlib.sha256(e.payload).hexdigest() if e.type == T_FILE else None,
            "payload_text": (e.payload.decode("ascii", "replace")
                             if e.type == T_FILE and e.kind == K_TEXT else None),
        })
    manifest = {
        "generator": "scripts/mkfs_vimtufs2_testimg.py",
        "format": "VimtuFS2",
        "format_source": "VimtuOS kernel/vfs64.h + kernel/vfs64.cpp (GPL-3.0); "
                         "generator written from the spec, not copied",
        "version": plan["version"],
        "feature_bits": plan["feature_bits"],
        "sector_bytes": SECTOR_BYTES,
        "block_bytes": BLOCK_BYTES,
        "inode_bytes": INODE_BYTES,
        "geometry": g,
        "free_blocks": plan["free_blocks"],
        "used_data_blocks": plan["used_data_blocks"],
        "sb_crc32": "%08x" % plan["sb_crc32"],
        "image": {
            "path": str(Path(image_path)),
            "size": len(image),
            "sha256": hashlib.sha256(image).hexdigest(),
        },
        "files": files,
    }
    manifest_path = Path(manifest_path)
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


# ============================================================================
# Independent verifier (--verify): parses the bytes again, trusts nothing
# ============================================================================

class Verify:
    def __init__(self):
        self.checks = []
        self.fails = []

    def ok(self, name, cond, detail=""):
        self.checks.append((name, bool(cond), detail))
        if not cond:
            self.fails.append(name)
        return bool(cond)

    def report(self, label):
        for name, good, detail in self.checks:
            print("  [%s] %s%s" % ("ok" if good else "FAIL", name,
                                   ("  (" + detail + ")") if detail and not good else ""))
        print("[%s] checks=%d ok=%d fail=%d" %
              (label, len(self.checks), len(self.checks) - len(self.fails),
               len(self.fails)))
        return 0 if not self.fails else 1


def read_block(image, blk):
    return image[blk * BLOCK_BYTES:(blk + 1) * BLOCK_BYTES]


def inode_at(image, g, idx):
    blk = g["inode_start"] + idx // INODES_PER_BLOCK
    off = (idx % INODES_PER_BLOCK) * INODE_BYTES
    start = blk * BLOCK_BYTES + off
    return image[start:start + INODE_BYTES]


def inode_blocks(image, g, raw, version):
    """Follow d0..d3 / ind / dind and return the mapped data blocks in order."""
    out = []
    nblk = (le32(raw, I_SIZE) + BLOCK_BYTES - 1) // BLOCK_BYTES
    for i in range(DIRECT_BLOCKS):
        out.append(le32(raw, I_D0 + 4 * i))
    ind = le32(raw, I_IND)
    if ind:
        table = read_block(image, ind)
        for i in range(INDIRECT_PTRS):
            out.append(le32(table, 4 * i))
    else:
        out.extend([0] * INDIRECT_PTRS)
    dind = le32(raw, I_DIND) if version >= VERSION_V3 else 0
    if dind:
        table = read_block(image, dind)
        for c in range(DIND_CHILDREN):
            child = le32(table, 4 * c)
            if child:
                ctable = read_block(image, child)
                for i in range(INDIRECT_PTRS):
                    out.append(le32(ctable, 4 * i))
            else:
                out.extend([0] * INDIRECT_PTRS)
    else:
        out.extend([0] * (DIND_CHILDREN * INDIRECT_PTRS))
    return out[:nblk] if nblk <= len(out) else out


def verify_image(image_path, manifest_path, label="vimtufs2-verify"):
    v = Verify()
    image_path = Path(image_path)
    manifest = json.loads(Path(manifest_path).read_text(encoding="utf-8"))
    image = image_path.read_bytes()

    # ---- image identity ----------------------------------------------------
    v.ok("image size matches the manifest (%d bytes)" % manifest["image"]["size"],
         len(image) == manifest["image"]["size"],
         "got %d" % len(image))
    digest = hashlib.sha256(image).hexdigest()
    v.ok("image sha256 matches the manifest", digest == manifest["image"]["sha256"],
         "%s != %s" % (digest, manifest["image"]["sha256"]))

    # ---- superblock --------------------------------------------------------
    sb = read_block(image, 0)
    v.ok("superblock magic is VIMTUFS2", sb[SB_MAGIC:SB_MAGIC + 8] == MAGIC,
         repr(bytes(sb[SB_MAGIC:SB_MAGIC + 8])))
    stored_crc = le32(sb, SB_CRC)
    v.ok("superblock CRC32 over [0,60) is valid",
         stored_crc == crc32(bytes(sb[:SB_CRC_LEN])),
         "%08x != %08x" % (stored_crc, crc32(bytes(sb[:SB_CRC_LEN]))))
    v.ok("superblock CRC32 matches the manifest (%08x)" % stored_crc,
         stored_crc == int(manifest["sb_crc32"], 16))
    v.ok("superblock ends with 0xAA55", le16(sb, SB_SIG) == 0xAA55,
         "%04x" % le16(sb, SB_SIG))
    version = le32(sb, SB_VERSION)
    v.ok("version is %d (manifest)" % manifest["version"], version == manifest["version"],
         "got %d" % version)
    v.ok("version is 3, 4 or 5", version in (VERSION_V3, VERSION_V4, VERSION_V5))
    v.ok("sector size is 512", le32(sb, SB_SECTOR) == SECTOR_BYTES)
    v.ok("block size is 512", le32(sb, SB_BLOCK) == BLOCK_BYTES)
    v.ok("inode size is 128", le32(sb, SB_INOSZ) == INODE_BYTES)

    total = le32(sb, SB_TOTAL)
    v.ok("total block count == image size / 512", total == len(image) // BLOCK_BYTES,
         "%d vs %d" % (total, len(image) // BLOCK_BYTES))
    g = geometry(total)
    v.ok("bitmap starts at block 1", le32(sb, SB_BITMAP) == g["bitmap_start"])
    v.ok("bitmap block count recomputed", le32(sb, SB_BITMAPN) == g["bitmap_blocks"],
         "%d vs %d" % (le32(sb, SB_BITMAPN), g["bitmap_blocks"]))
    v.ok("inode start recomputed", le32(sb, SB_INODE) == g["inode_start"],
         "%d vs %d" % (le32(sb, SB_INODE), g["inode_start"]))
    v.ok("inode count is total/16 (clamped 16..2048)",
         le32(sb, SB_INODEN) == g["inodes"],
         "%d vs %d" % (le32(sb, SB_INODEN), g["inodes"]))
    v.ok("data start recomputed", le32(sb, SB_DATA) == g["data_start"],
         "%d vs %d" % (le32(sb, SB_DATA), g["data_start"]))
    v.ok("data block count recomputed", le32(sb, SB_DATAN) == g["data_blocks"],
         "%d vs %d" % (le32(sb, SB_DATAN), g["data_blocks"]))
    v.ok("root inode is 0 and inside the inode table", le32(sb, SB_ROOT) == 0)
    v.ok("reserved flags field is 0", le32(sb, SB_FLAGS) == 0)
    feat = le32(sb, SB_FEATBITS)
    v.ok("feature bits match the manifest (0x%x)" % manifest["feature_bits"],
         feat == manifest["feature_bits"], "got 0x%x" % feat)
    if feat:
        v.ok("feature header CRC32 over [64,84) is valid",
             le32(sb, SB_FEATCRC) == crc32(bytes(sb[SB_FEATBITS:SB_FEATBITS + SB_FEAT_CRC_LEN])))
        v.ok("feature bits are all known", (feat & ~FEAT_ALL) == 0)
        v.ok("feature table block numbers are inside the data area",
             (feat & FEAT_JOURNAL) == 0 or
             g["data_start"] <= le32(sb, SB_JX_START) < total)
    else:
        v.ok("feature extension header is all zero for a feature-free volume",
             bytes(sb[SB_FEATBITS:SB_FEATBITS + 24]) == b"\0" * 24)
    v.ok("bytes [88,510) of the superblock are zero",
         bytes(sb[88:510]) == b"\0" * (510 - 88))

    # ---- bitmap ------------------------------------------------------------
    bitmap = image[g["bitmap_start"] * BLOCK_BYTES:
                   (g["bitmap_start"] + g["bitmap_blocks"]) * BLOCK_BYTES]
    bit = lambda b: (bitmap[b >> 3] >> (b & 7)) & 1

    def bit_is(blk, want):
        return bit(blk) == (1 if want else 0)

    v.ok("every metadata block (0..data_start-1) is marked used",
         all(bit_is(b, True) for b in range(g["data_start"])))
    v.ok("padding bits beyond the volume are marked used",
         all(bit_is(b, True) for b in range(total,
                                           g["bitmap_blocks"] * BITMAP_BLOCK_BITS)))

    # ---- inode table: structural validation --------------------------------
    reserved_start = {VERSION_V3: I_UID, VERSION_V4: I_XATTR, VERSION_V5: I_SNAPF + 4}
    rsvd_from = reserved_start[version]
    live = []
    bad = []
    for idx in range(g["inodes"]):
        raw = inode_at(image, g, idx)
        t = raw[I_TYPE]
        if t == T_FREE:
            continue
        live.append(idx)
        where = "inode %d" % idx
        if not v.ok(where + ": type is 1/2/3", t in (T_FILE, T_DIR, T_LINK), str(t)):
            bad.append(idx)
            continue
        v.ok(where + ": namelen <= 31", raw[I_NAMELEN] <= NAME_MAX, str(raw[I_NAMELEN]))
        v.ok(where + ": reserved u16 is 0", le16(raw, I_RSVD) == 0)
        v.ok(where + ": rsvd2 is 0", raw[I_RSVD2] == 0)
        want_crc = crc32(bytes(raw[:I_CRC]))
        v.ok(where + ": inode CRC32 over [0,124)",
             le32(raw, I_CRC) == want_crc,
             "%08x != %08x" % (le32(raw, I_CRC), want_crc))
        v.ok(where + ": kind <= 6", raw[I_KIND] <= 6, str(raw[I_KIND]))
        v.ok(where + ": nlink != 0", le16(raw, I_NLINK) != 0)
        v.ok(where + ": reserved bytes are zero",
             bytes(raw[rsvd_from:I_CRC]) == b"\0" * (I_CRC - rsvd_from))
        v.ok(where + ": namelen matches the stored name",
             raw[I_NAMELEN] == len(raw[I_NAME:I_NAME + NAME_MAX].rstrip(b"\0")))
        if version >= VERSION_V4:
            mode = le16(raw, I_MODE)
            want = S_IFDIR if t == T_DIR else (S_IFLNK if t == T_LINK else S_IFREG)
            v.ok(where + ": mode type bits match the inode type",
                 (mode & S_IFMT) == want, "mode=0o%o" % mode)
            v.ok(where + ": mode has no bits outside 0777",
                 (mode & ~(S_IFMT | 0o777)) == 0, "mode=0o%o" % mode)
        mt = le32(raw, I_MTIME)
        if mt:
            v.ok(where + ": packed mtime fields are in range",
                 1 <= ((mt >> 22) & 0xF) <= 12 and 1 <= ((mt >> 17) & 0x1F) <= 31 and
                 ((mt >> 12) & 0x1F) <= 23 and ((mt >> 6) & 0x3F) <= 59 and
                 (mt & 0x3F) <= 59)
        if t == T_DIR:
            v.ok(where + ": directory has size 0 and no data blocks",
                 le32(raw, I_SIZE) == 0 and
                 all(le32(raw, I_D0 + 4 * i) == 0 for i in range(DIRECT_BLOCKS)) and
                 le32(raw, I_IND) == 0 and le32(raw, I_DIND) == 0)
        if t == T_FILE:
            size = le32(raw, I_SIZE)
            v.ok(where + ": file size <= 8388608", size <= MAX_FILE_BLOCKS * BLOCK_BYTES)
            nblk = (size + BLOCK_BYTES - 1) // BLOCK_BYTES
            v.ok(where + ": file needs the indirect block it claims",
                 nblk <= DIRECT_BLOCKS or le32(raw, I_IND) != 0)
            v.ok(where + ": file needs the double indirect block it claims",
                 nblk <= L2_FIRST_BLOCK or le32(raw, I_DIND) != 0)
            for name, blk in (("ind", le32(raw, I_IND)), ("dind", le32(raw, I_DIND))):
                if blk:
                    v.ok(where + ": %s block is inside the data area" % name,
                         g["data_start"] <= blk < total, str(blk))

    # ---- used set: exactly the referenced blocks ---------------------------
    referenced = set()
    problems = []

    def ref(blk, what):
        if blk == 0:
            return
        if not (g["data_start"] <= blk < total):
            problems.append("%s: block %d outside the data area" % (what, blk))
            return
        if blk in referenced:
            problems.append("%s: block %d referenced twice" % (what, blk))
        referenced.add(blk)

    for idx in live:
        raw = inode_at(image, g, idx)
        if raw[I_TYPE] != T_FILE:
            continue
        size = le32(raw, I_SIZE)
        nblk = (size + BLOCK_BYTES - 1) // BLOCK_BYTES
        mapped = inode_blocks(image, g, raw, version)
        v.ok("inode %d: mapped block count covers the file size" % idx,
             len(mapped) == nblk, "%d mapped, %d needed" % (len(mapped), nblk))
        for i, blk in enumerate(mapped):
            if blk == 0:
                problems.append("inode %d: fs block %d is not mapped" % (idx, i))
                continue
            ref(blk, "inode %d fs block %d" % (idx, i))
        if le32(raw, I_IND):
            ref(le32(raw, I_IND), "inode %d ind" % idx)
        if le32(raw, I_DIND):
            ref(le32(raw, I_DIND), "inode %d dind" % idx)
            table = read_block(image, le32(raw, I_DIND))
            for c in range(DIND_CHILDREN):
                child = le32(table, 4 * c)
                if child:
                    ref(child, "inode %d dind child %d" % (idx, c))
    v.ok("no data block is referenced twice and all are inside the data area",
         not problems, "; ".join(problems[:4]))
    used = {b for b in range(g["data_start"], total) if bit(b)}
    v.ok("bitmap used set == the block set the inode tree references",
         used == referenced,
         "bitmap-only=%s tree-only=%s" % (sorted(used - referenced)[:4],
                                          sorted(referenced - used)[:4]))
    v.ok("free block count == manifest",
         len(used) == plan_used_count(manifest),
         "%d vs %d" % (len(used), plan_used_count(manifest)))
    v.ok("free block count == data_blocks - used",
         manifest["free_blocks"] == g["data_blocks"] - len(used))

    # ---- directory tree + per-file content --------------------------------
    parent_of = {}
    name_of = {}
    type_of = {}
    for idx in live:
        raw = inode_at(image, g, idx)
        parent_of[idx] = le32(raw, I_PARENT)
        name_of[idx] = bytes(raw[I_NAME:I_NAME + raw[I_NAMELEN]]).decode("ascii", "replace")
        type_of[idx] = raw[I_TYPE]
    root = le32(sb, SB_ROOT)
    v.ok("root inode has an empty name and points at itself",
         name_of.get(root, "") == "" and parent_of.get(root, None) == root)
    root_subdirs = sum(1 for i in live
                       if type_of[i] == T_DIR and parent_of[i] == root and i != root)
    v.ok("root nlink == 1 + number of direct subdirectories",
         le16(inode_at(image, g, root), I_NLINK) == 1 + root_subdirs,
         "%d vs %d" % (le16(inode_at(image, g, root), I_NLINK), 1 + root_subdirs))
    for idx in live:
        if idx == root:
            continue
        if not v.ok("inode %d: parent is a live directory" % idx,
                    parent_of.get(idx) in type_of and type_of[parent_of[idx]] == T_DIR,
                    str(parent_of.get(idx))):
            continue
        # parent chain must terminate at the root without a cycle
        seen = set()
        cur = idx
        chain_ok = True
        while cur != root:
            if cur in seen or cur not in parent_of:
                chain_ok = False
                break
            seen.add(cur)
            cur = parent_of[cur]
        v.ok("inode %d: parent chain reaches the root" % idx, chain_ok)

    # every manifest entry must resolve exactly as recorded
    for rec in manifest["files"]:
        idx = rec["inode"]
        if not v.ok("manifest entry %r exists at inode %d" % (rec["name"], idx),
                    idx in type_of):
            continue
        raw = inode_at(image, g, idx)
        v.ok("%r: name/parent/type as recorded" % rec["name"],
             name_of[idx] == rec["name"] and parent_of[idx] == rec["parent"] and
             (("dir" if type_of[idx] == T_DIR else "file") == rec["type"]),
             "name=%r parent=%d type=%d" % (name_of[idx], parent_of[idx], type_of[idx]))
        if rec["type"] != "file":
            continue
        v.ok("%r: size as recorded" % rec["name"],
             le32(raw, I_SIZE) == rec["size"],
             "%d vs %d" % (le32(raw, I_SIZE), rec["size"]))
        content = bytearray()
        for blk in inode_blocks(image, g, raw, version):
            content += read_block(image, blk)
        content = bytes(content[:rec["size"]])
        v.ok("%r: content sha256 as recorded" % rec["name"],
             hashlib.sha256(content).hexdigest() == rec["sha256"])
        if rec["size"] >= 300 * BLOCK_BYTES:
            v.ok("%r: payload follows the generator pattern (block order proven)"
                 % rec["name"], content == big2_payload(rec["size"]))

    # the fixture must actually exercise the indirect layers
    big = next((r for r in manifest["files"] if r["fs_blocks"] > L2_FIRST_BLOCK), None)
    v.ok("fixture has a file that needs the double indirect block", big is not None)
    if big:
        v.ok("double-indirect file uses >= 2 child blocks",
             len(big["dind_child_blocks"]) >= 2,
             "%d children" % len(big["dind_child_blocks"]))
        v.ok("double-indirect file uses both direct and single indirect too",
             big["fs_blocks"] > L2_FIRST_BLOCK and big["ind_block"] != 0)
    return v.report(label)


def plan_used_count(manifest):
    return len(manifest["used_data_blocks"])


def main(argv=None):
    ap = argparse.ArgumentParser(description="Generate a VimtuFS2 test volume")
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--manifest", default=str(DEFAULT_MANIFEST))
    ap.add_argument("--version", type=int, default=VERSION_V5,
                    choices=[VERSION_V3, VERSION_V4, VERSION_V5])
    ap.add_argument("--size-mib", type=int, default=12)
    ap.add_argument("--features", type=lambda s: int(s, 0), default=0,
                    help="feature bits to set in a v5 superblock (negative fixture)")
    ap.add_argument("--verify", action="store_true",
                    help="re-parse --out/--manifest independently and check them")
    args = ap.parse_args(argv)

    if args.verify:
        if not Path(args.out).exists() or not Path(args.manifest).exists():
            print("nothing to verify: %s / %s" % (args.out, args.manifest))
            return 1
        return verify_image(args.out, args.manifest)

    total = args.size_mib * 1024 * 1024 // BLOCK_BYTES
    image, plan = build_image(total, args.version, args.features)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(image)
    manifest = make_manifest(image, plan, out, args.manifest)
    g = plan["geometry"]
    print("[mkfs-vimtufs2] %s: version=%d size=%d bytes blocks=%d" %
          (out, plan["version"], len(image), g["blocks"]))
    print("[mkfs-vimtufs2] geometry: bitmap=%d+%d inode=%d+%d data=%d+%d inodes=%d" %
          (g["bitmap_start"], g["bitmap_blocks"], g["inode_start"],
           g["inode_blocks"], g["data_start"], g["data_blocks"], g["inodes"]))
    print("[mkfs-vimtufs2] sb crc32=%08x free_blocks=%d used=%d" %
          (plan["sb_crc32"], plan["free_blocks"], len(plan["used_data_blocks"])))
    for e in plan["entries"]:
        print("[mkfs-vimtufs2]   inode %d %-32s parent=%d %s size=%d blocks=%d%s%s" %
              (e.index, e.name or "<root>", e.parent,
               "dir " if e.type == T_DIR else "file", e.size, e.fs_blocks,
               (", ind=%d" % e.ind) if e.ind else "",
               (", dind=%d children=%d" % (e.dind, len(e.children))) if e.dind else ""))
    print("[mkfs-vimtufs2] sha256=%s" % manifest["image"]["sha256"])
    print("[mkfs-vimtufs2] manifest=%s" % args.manifest)
    if args.features:
        print("[mkfs-vimtufs2] NOTE: feature bits 0x%x set - this is a NEGATIVE "
              "fixture a conforming v4/v5 read-only driver must refuse" % args.features)
    print("[mkfs-vimtufs2] OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
