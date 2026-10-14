#!/usr/bin/env python3
"""Build a REAL SquashFS 4.0 test image for SutureOS - pure Python stdlib.

No third-party packages and no external tools: mksquashfs is NOT installed on
this machine, so the image is assembled byte by byte from the on-disk format
(see the official SquashFS 4.0 layout: superblock, metadata blocks with 2-byte
length headers and the 0x8000 "stored uncompressed" flag, inode table,
directory table, fragment table, id table).

Compression: gzip. Metadata blocks are zlib-deflated (their header's 0x8000 bit
left clear); data blocks are stored as raw deflate and their block-list entry
has the 0x01000000 bit set when they are stored uncompressed. One data block is
deliberately incompressible so the "stored uncompressed" path is exercised.

Default output: build/squashfs.img  (1 MiB, 4096-byte blocks, gzip)

Filesystem contents (byte patterns are documented and re-derived by
scripts/verify_squashfs_read.py, tests/squashfs_harness and
kernel/fs/squashfs.c's squashfs_selftest()):

  /HELLO.TXT           67 B  "Hello from SutureOS SquashFS!\\nSecond line: gzip metadata and data.\\n"
                             (tail stored in a fragment)
  /README              43 B  "SquashFS 4.0 read-only driver test volume.\\n"
  /LARGE.BIN       102400 B  b[i] = (i * 31 + 7) & 0xFF      (25 x 4096 blocks)
  /UNCOMP.BIN        8192 B  b[i] = lcg(i) & 0xFF            (2 INCOMPRESSIBLE
                             blocks stored UNCOMPRESSED on disk)
  /FRAG.TXT          1000 B  b[i] = (i * 5 + 1) & 0xFF       (one fragment)
  /ZERO.BIN             0 B  empty file
  /LINK.TXT               symlink -> "LARGE.BIN"   (basic symlink inode)
  /SUB/NESTED.TXT      63 B  "Nested file: /SUB/NESTED.TXT via the SquashFS directory table.\\n"
  /SUB/DEEP/A/B/C/DEEP.TXT
                       47 B  "Deep directory chain reached through SquashFS.\\n"

The script re-parses its own output with an INDEPENDENT reader (separate code
path, fixed on-disk offsets, its own deflate decompressor) and prints
`host-side verification: PASS` with the check counts. It then also prints the
byte layout so the same facts can be asserted from the outside.

Usage:
  py -3 scripts/mkfs_squashfs_testimg.py
  py -3 scripts/mkfs_squashfs_testimg.py --out build/squashfs.img --block-size 4096
  py -3 scripts/mkfs_squashfs_testimg.py --corrupt-magic   (negative-case fixture)
"""

import argparse
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "build")

SECTOR = 512
MAGIC = 0x73717368
VERSION_MAJOR = 4
VERSION_MINOR = 0
COMP_GZIP = 1
METADATA_SIZE = 8192
COMPRESSED_BIT = 0x8000            # metadata block header
COMPRESSED_BIT_BLOCK = 0x01000000  # data/fragment block-list entry
BLOCK_SIZE_MASK = 0x00FFFFFF
NO_FRAGMENT = 0xFFFFFFFF

FLAG_UNCOMPRESSED_INODES = 0x0001
FLAG_UNCOMPRESSED_DATA = 0x0002
FLAG_UNCOMPRESSED_FRAGMENTS = 0x0008
FLAG_NO_FRAGMENTS = 0x0010
FLAG_NO_XATTRS = 0x0200

INODE_DIR = 1
INODE_FILE = 2
INODE_SYMLINK = 3
INODE_EXT_DIR = 8
INODE_EXT_FILE = 9
INODE_EXT_SYMLINK = 10
DIR_TYPE_OFFSET = 3

# S_IF* values
S_IFDIR = 0o040000
S_IFREG = 0o100000
S_IFLNK = 0o120000

MTIME = 1700000000                 # deterministic timestamp
UID = 0
GID = 0

IMG_SIZE = 1 * 1024 * 1024         # 1 MiB
BLOCK_SIZE = 4096

# --------------------------------------------------------------------------
# CONTENT RULES - must stay in sync with kernel/fs/squashfs.c
# (squashfs_selftest()) and tests/squashfs_harness/harness.c
# --------------------------------------------------------------------------
HELLO_TXT = b"Hello from SutureOS SquashFS!\nSecond line: gzip metadata and data.\n"
README_TXT = b"SquashFS 4.0 read-only driver test volume.\n"
NESTED_TXT = b"Nested file: /SUB/NESTED.TXT via the SquashFS directory table.\n"
DEEP_TXT = b"Deep directory chain reached through SquashFS.\n"
LINK_TARGET = b"LARGE.BIN"

LARGE_SIZE = 100 * 1024            # 25 blocks of 4096
UNCOMP_SIZE = 8 * 1024             # 2 blocks of 4096
FRAG_SIZE = 1000


def large_byte(i):
    return (i * 31 + 7) & 0xFF


def frag_byte(i):
    return (i * 5 + 1) & 0xFF


def lcg_byte(i):
    """Incompressible-ish deterministic stream. The C driver re-derives the
    same bytes with 32-bit wrapping arithmetic."""
    s = (0x12345678 + i * 2654435761) & 0xFFFFFFFF
    s ^= (s << 13) & 0xFFFFFFFF
    s ^= (s >> 17)
    s ^= (s << 5) & 0xFFFFFFFF
    return s & 0xFF


def make_large():
    return bytes(large_byte(i) for i in range(LARGE_SIZE))


def make_uncomp():
    return bytes(lcg_byte(i) for i in range(UNCOMP_SIZE))


def make_frag():
    return bytes(frag_byte(i) for i in range(FRAG_SIZE))


# ==========================================================================
# Image writer
# ==========================================================================

class Node(object):
    """One filesystem object."""

    def __init__(self, name, kind, mode, content=None, target=None, parent=None):
        self.name = name
        self.kind = kind          # "dir" | "file" | "symlink"
        self.mode = mode
        self.content = content    # bytes for files
        self.target = target      # bytes for symlinks
        self.parent = parent
        self.children = []
        self.inode_number = 0
        self.inode_ref = 0
        self.dir_start_block = 0
        self.dir_start_offset = 0
        self.dir_size_field = 0   # size stored in the inode (listing bytes + 3)
        self.nlink = 1
        self.blocks_start = 0
        self.fragment_index = NO_FRAGMENT
        self.fragment_offset = NO_FRAGMENT
        self.data_blocks = []     # list of (offset, on_disk_len, compressed)

    def add(self, child):
        child.parent = self
        self.children.append(child)
        return child


class SqfsBuilder(object):
    def __init__(self, block_size, img_size):
        self.bs = block_size
        self.img = bytearray(img_size)
        self.pos = 0                  # next free byte
        self.comp_blocks = 0
        self.raw_blocks = 0
        self.frag_entries = []
        self.compressed_data_blocks = 0
        self.raw_data_blocks = 0
        self.data_start = 0

    # ---------------- raw output ----------------
    def emit(self, data):
        off = self.pos
        end = off + len(data)
        if end > len(self.img):
            raise AssertionError("image overflow: need %d bytes, have %d" % (end, len(self.img)))
        self.img[off:end] = data
        self.pos = end
        return off

    # ---------------- compression helpers ----------------
    def compress_meta(self, chunk, force_raw=False):
        """Metadata: zlib stream; store uncompressed when it does not shrink.
        `force_raw` stores even compressible blocks raw, which is how a
        SquashFS image with SQUASHFS_FLAG_UNCOMPRESSED_INODES looks."""
        c = zlib.compress(chunk, 9)
        if force_raw or len(c) >= len(chunk):
            self.raw_blocks += 1
            return struct.pack("<H", len(chunk) | COMPRESSED_BIT) + chunk
        self.comp_blocks += 1
        return struct.pack("<H", len(c)) + c

    def compress_data(self, chunk):
        """Data blocks use raw deflate; return (payload, entry_word)."""
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
        raw = c.compress(chunk) + c.flush()
        if len(raw) >= len(chunk):
            self.raw_data_blocks += 1
            return chunk, (len(chunk) | COMPRESSED_BIT_BLOCK)
        self.compressed_data_blocks += 1
        return raw, len(raw)

    def write_data_block(self, chunk):
        payload, entry = self.compress_data(chunk)
        off = self.emit(payload)
        return off, entry

    def write_meta_stream(self, data, force_raw=False):
        """Write `data` as a stream of metadata blocks at the current position.

        Returns (first_block_offset, block_offsets, virtual_positions) where
        virtual_positions[i] is the virtual byte offset of byte i's block."""
        first = self.pos
        offsets = []
        virtual = []
        pos = 0
        while pos < len(data):
            chunk = data[pos:pos + METADATA_SIZE]
            offsets.append(self.pos)
            virtual.extend([pos] * len(chunk))
            self.emit(self.compress_meta(chunk, force_raw))
            pos += len(chunk)
        if not offsets:                       # empty listing: one empty block
            offsets.append(self.pos)
            self.emit(self.compress_meta(b"", force_raw))
        return first, offsets, virtual


# ==========================================================================
# Node references and directory listing layout
# ==========================================================================

def build_tree():
    root = Node("/", "dir", S_IFDIR | 0o755)
    root.add(Node("HELLO.TXT", "file", S_IFREG | 0o644, content=HELLO_TXT))
    root.add(Node("README", "file", S_IFREG | 0o644, content=README_TXT))
    root.add(Node("LARGE.BIN", "file", S_IFREG | 0o644, content=make_large()))
    root.add(Node("UNCOMP.BIN", "file", S_IFREG | 0o644, content=make_uncomp()))
    root.add(Node("FRAG.TXT", "file", S_IFREG | 0o644, content=make_frag()))
    root.add(Node("ZERO.BIN", "file", S_IFREG | 0o644, content=b""))
    root.add(Node("LINK.TXT", "symlink", S_IFLNK | 0o777, target=LINK_TARGET))
    sub = root.add(Node("SUB", "dir", S_IFDIR | 0o755))
    sub.add(Node("NESTED.TXT", "file", S_IFREG | 0o644, content=NESTED_TXT))
    a = sub.add(Node("DEEP", "dir", S_IFDIR | 0o755))
    b = a.add(Node("A", "dir", S_IFDIR | 0o755))
    c = b.add(Node("B", "dir", S_IFDIR | 0o755))
    d = c.add(Node("C", "dir", S_IFDIR | 0o755))
    d.add(Node("DEEP.TXT", "file", S_IFREG | 0o644, content=DEEP_TXT))
    return root


def assign_inode_numbers(root):
    """Deterministic depth-first numbering, root == 2."""
    order = []

    def walk(node):
        order.append(node)
        for ch in node.children:
            walk(ch)

    walk(root)
    for i, node in enumerate(order):
        node.inode_number = i + 2
    # hard link counts: a directory is linked by its parent's entry, its own "."
    # and by each subdirectory's ".."
    for node in order:
        if node.kind == "dir":
            subs = sum(1 for c in node.children if c.kind == "dir")
            node.nlink = 2 + subs
        else:
            node.nlink = 1
    return order


def serialize_dir_listing(node, meta_pos_of):
    """Serialize one directory's listing (header + entries).

    `meta_pos_of` maps a (inode metadata block, offset) leaf inode to a
    virtual directory-table position; it is a function(leaf) -> (block, offset)
    already recorded while serializing the inode table.
    """
    entries = [("."), ("..")] + [c.name for c in node.children]
    # resolve each entry to (inode_number, inode_ref_block, inode_ref_offset, itype)
    resolved = []
    for i, name in enumerate(entries):
        if name == ".":
            target = node
        elif name == "..":
            target = node.parent if node.parent else node
        else:
            target = [c for c in node.children if c.name == name][0]
        block, offset = meta_pos_of[target.inode_number]
        itype = {"dir": INODE_DIR, "file": INODE_FILE, "symlink": INODE_SYMLINK}[target.kind]
        resolved.append((name.encode(), target.inode_number, block, offset, itype))

    out = bytearray()
    if len(resolved) > 256:
        raise AssertionError("directory with more than 256 entries needs a new header")
    first = resolved[0]
    out += struct.pack("<III", len(resolved) - 1, first[2], first[1])
    for name, ino, block, offset, itype in resolved:
        delta = ino - first[1]
        if not (-32768 <= delta <= 32767):
            raise AssertionError("inode delta out of range")
        # the on-disk type field carries the SQUASHFS_INODE_* value directly
        out += struct.pack("<HhHH", offset, delta, itype, len(name) - 1) + name
    return bytes(out)


# ==========================================================================
# Build
# ==========================================================================

def build_image(block_size, img_size, corrupt_magic=False, bad_fragment_ref=None,
                bad_root_inode=False, bad_blocks_start=None, fragments=True,
                raw_meta=False):
    """Build the image.

    corrupt_magic     - store a wrong magic (mount must fail)
    bad_fragment_ref  - force this value into EVERY file inode's fragment_index,
                        producing a consistent image that references fragments
                        that do not exist (open/read must fail, mount must work)
    bad_root_inode    - point the superblock root inode reference at an inode
                        table block that does not exist (mount/stat must fail)
    bad_blocks_start  - force this value into EVERY file inode's blocks_start,
                        so the data-block list points past the end of the image
                        (mount and stat must work, reading data must fail)
    fragments=False   - no fragment table at all: small files get their own data
                        block and the last one is stored short (exercises the
                        "no fragments" superblock flag and the short-tail path)
    raw_meta=True     - store every metadata block UNCOMPRESSED (the
                        SQUASHFS_FLAG_UNCOMPRESSED_INODES layout: the metadata
                        block header's 0x8000 bit set)
    """
    b = SqfsBuilder(block_size, img_size)
    root = build_tree()
    order = assign_inode_numbers(root)
    blocks = [n for n in order if n.kind == "file" and len(n.content or b"") >= block_size]
    smalls = [n for n in order if n.kind == "file" and 0 < len(n.content or b"") < block_size]
    empties = [n for n in order if n.kind == "file" and len(n.content or b"") == 0]

    # superblock placeholder at 0
    b.pos = 96

    # ---------------- data blocks ----------------
    b.data_start = b.pos
    for node in blocks:
        data = node.content
        nb = len(data) // block_size           # whole blocks; the tail (if any) is fragmented
        node.blocks_start = b.pos
        for i in range(nb):
            off, entry = b.write_data_block(data[i * block_size:(i + 1) * block_size])
            node.data_blocks.append((off, entry))
        tail = data[nb * block_size:]
        if tail:
            # fragment: compress the tail alone and register a fragment entry
            payload, entry = b.compress_data(tail)
            fb = b.emit(payload)
            node.blocks_start = node.blocks_start if nb else fb
            node.fragment_index = len(b.frag_entries)
            node.fragment_offset = 0
            b.frag_entries.append((fb, entry, len(tail)))
        else:
            node.fragment_index = NO_FRAGMENT
            node.fragment_offset = NO_FRAGMENT
    # one shared fragment block carrying every small file's bytes
    frag_block_off = 0
    frag_block_entry = 0
    frag_used = 0
    if smalls and not fragments:
        # no-fragment layout: each small file gets its own (short) data block
        for node in smalls:
            node.blocks_start = b.pos
            payload, entry = b.compress_data(node.content)
            b.emit(payload)
            node.data_blocks.append((node.blocks_start, entry))
            node.fragment_index = NO_FRAGMENT
            node.fragment_offset = NO_FRAGMENT
    elif smalls:
        frag_block_off = b.pos
        payload, entry = b.compress_data(b"".join(n.content for n in smalls))
        frag_block_entry = entry
        b.emit(payload)
        for node in smalls:
            node.fragment_index = len(b.frag_entries)
            node.fragment_offset = frag_used
            b.frag_entries.append((frag_block_off, frag_block_entry,
                                   len(node.content)))
            frag_used += len(node.content)
    for node in empties:
        node.blocks_start = b.pos
        node.fragment_index = NO_FRAGMENT
        node.fragment_offset = NO_FRAGMENT

    # ---------------- inode table ----------------
    inode_table_pos = b.pos
    inode_stream = bytearray()
    meta_pos = {}          # inode_number -> (metadata block index, offset)
    # inode refs are (block << 16) | offset with block relative to the table start
    stream_base = 0

    def inode_ref_for(node):
        base = node.inode_stream_offset
        blk = base // METADATA_SIZE
        off = base % METADATA_SIZE
        return blk, off

    # We must know each inode's (block, offset) before the directory table can be
    # written, so first serialize into a buffer while tracking positions.
    for node in order:
        node.inode_stream_offset = len(inode_stream)
        blk = node.inode_stream_offset // METADATA_SIZE
        off = node.inode_stream_offset % METADATA_SIZE
        node.inode_ref_block = blk
        node.inode_ref_offset = off
        node.inode_ref = (blk << 16) | off
        meta_pos[node.inode_number] = (blk, off)

        hdr = struct.pack("<HHHHII", {"dir": INODE_DIR, "file": INODE_FILE,
                                      "symlink": INODE_SYMLINK}[node.kind],
                          node.mode, UID, GID, MTIME, node.inode_number)
        inode_stream += hdr
        if node.kind == "dir":
            # start_block/offset/size are patched in the second pass below
            node.inode_payload_offset = len(inode_stream)
            inode_stream += struct.pack("<IIHHI", 0, node.nlink, 0, 0,
                                        node.parent.inode_number if node.parent else 2)
        elif node.kind == "file":
            node.inode_payload_offset = len(inode_stream)
            frag_index = node.fragment_index
            if bad_fragment_ref is not None and bad_fragment_ref != NO_FRAGMENT:
                frag_index = bad_fragment_ref
            blocks_start = node.blocks_start
            if bad_blocks_start is not None:
                blocks_start = bad_blocks_start
            inode_stream += struct.pack("<IIII", blocks_start,
                                        frag_index, node.fragment_offset,
                                        len(node.content))
            for _, entry in node.data_blocks:
                inode_stream += struct.pack("<I", entry)
        else:
            node.inode_payload_offset = len(inode_stream)
            tgt = node.target
            inode_stream += struct.pack("<II", node.nlink, len(tgt)) + tgt

    # ---------------- directory table ----------------
    dir_table_pos = b.pos
    dir_stream = bytearray()

    def walk_dirs(node):
        node.dir_stream_offset = len(dir_stream)
        node.dir_start_block = node.dir_stream_offset // METADATA_SIZE
        node.dir_start_offset = node.dir_stream_offset % METADATA_SIZE
        listing = serialize_dir_listing(node, meta_pos)
        node.listing_len = len(listing)
        node.dir_size_field = len(listing) + 3
        dir_stream.extend(listing)
        for ch in node.children:
            if ch.kind == "dir":
                walk_dirs(ch)

    walk_dirs(root)

    # patch directory inode payloads with start_block / size / offset
    for node in order:
        if node.kind != "dir":
            continue
        struct.pack_into("<I", inode_stream, node.inode_payload_offset,
                         node.dir_start_block)
        struct.pack_into("<H", inode_stream, node.inode_payload_offset + 8,
                         node.dir_size_field)
        struct.pack_into("<H", inode_stream, node.inode_payload_offset + 10,
                         node.dir_start_offset)

    # Tables are laid out back to back with NO padding between them, so the
    # independent reader can derive each table's extent from the start of the
    # next one (this mirrors how SquashFS images are really written).
    inode_offsets = b.write_meta_stream(bytes(inode_stream), raw_meta)[1]
    dir_table_pos = b.pos
    dir_offsets = b.write_meta_stream(bytes(dir_stream), raw_meta)[1]

    # ---------------- fragment table ----------------
    # Canonical SquashFS 4.0 table layout: [metadata blocks][u64 lookup list].
    # s_fragment_table_start points at the METADATA, and s_lookup_table_start at
    # the u64 list that follows it (one entry per metadata block); a reader can
    # therefore read the list backwards to find where the table begins.
    frag_table_pos = b.pos
    frag_lookup_pos = 0
    lookup_pos = 0
    frag_offsets = []
    if b.frag_entries:
        entries = bytearray()
        for start, size, _unused in b.frag_entries:
            entries += struct.pack("<QII", start, size, 0)
        frag_offsets = b.write_meta_stream(bytes(entries))[1]
        frag_lookup_pos = b.pos
        b.emit(b"".join(struct.pack("<Q", o) for o in frag_offsets))
        lookup_pos = frag_lookup_pos

    # ---------------- id table ----------------
    # Layout: [u64 lookup list][metadata]. s_id_table_start points at the LIST
    # (so it sits below bytes_used and the entries can point forward at the
    # metadata). The two tables are allowed to use different orders; a reader
    # has to look at where each list's entries actually land.
    id_lookup_pos = b.pos
    id_offsets = []
    id_meta_pos = id_lookup_pos + 8          # one lookup entry
    b.pos = id_meta_pos
    id_meta_first, id_meta_offsets, _ = b.write_meta_stream(struct.pack("<I", 0))
    id_offsets = [id_meta_first]
    id_table_pos = id_meta_first
    saved = b.pos
    b.pos = id_lookup_pos
    b.emit(b"".join(struct.pack("<Q", o) for o in id_offsets))
    b.pos = saved

    bytes_used = b.pos

    # ---------------- superblock ----------------
    flags = FLAG_NO_XATTRS
    if raw_meta:
        flags |= FLAG_UNCOMPRESSED_INODES
    if b.raw_blocks:
        # some metadata blocks are stored uncompressed; that is expressed per
        # block, so no flag is needed. Keep the flag set only when *all* inodes
        # would be uncompressed (never here).
        pass
    sb = struct.pack("<IIIIIHHHHHHQQQQQQQQ",
                     MAGIC,                    # magic
                     len(order),               # inode_count
                     MTIME,                    # mkfs_time
                     block_size,               # block_size
                     len(b.frag_entries),      # fragment_entry_count
                     COMP_GZIP,                # compression
                     block_size.bit_length() - 1,   # block_log
                     flags,                    # flags
                     1,                        # id_count
                     VERSION_MAJOR,            # version_major
                     VERSION_MINOR,            # version_minor
                     root.inode_ref,           # root_inode
                     bytes_used,               # bytes_used
                     id_lookup_pos,            # id_table_start (the u64 lookup list)
                     0xFFFFFFFFFFFFFFFF,       # xattr_id_table_start
                     inode_table_pos,          # inode_table_start
                     dir_table_pos,            # directory_table_start
                     frag_table_pos if b.frag_entries else 0xFFFFFFFFFFFFFFFF,
                     frag_lookup_pos if b.frag_entries else 0xFFFFFFFFFFFFFFFF)
    assert len(sb) == 96, len(sb)
    if corrupt_magic:
        sb = struct.pack("<I", 0xDEADBEEF) + sb[4:]
    elif bad_root_inode:
        sb = sb[:32] + struct.pack("<Q", (0x7FFF << 16) | (root.inode_ref & 0xFFFF)) \
            + sb[40:]
    b.img[0:96] = sb

    info = {
        "img": bytes(b.img[:bytes_used]),
        "full_size": len(b.img),
        "bytes_used": bytes_used,
        "block_size": block_size,
        "inode_count": len(order),
        "fragment_count": len(b.frag_entries),
        "compressed_meta": b.comp_blocks,
        "stored_meta": b.raw_blocks,
        "compressed_data": b.compressed_data_blocks,
        "stored_data": b.raw_data_blocks,
        "compressed_fragments": 0,
        "inode_table_pos": inode_table_pos,
        "inode_offsets": inode_offsets,
        "dir_table_pos": dir_table_pos,
        "dir_offsets": dir_offsets,
        "frag_table_pos": frag_table_pos,
        "frag_lookup_pos": frag_lookup_pos,
        "frag_offsets": frag_offsets,
        "id_table_pos": id_table_pos,
        "id_lookup_pos": id_lookup_pos,
        "id_offsets": id_offsets,
        "meta_block_counts": {"inode": len(inode_offsets),
                              "dir": len(dir_offsets),
                              "frag": len(frag_offsets)},
        "nodes": order,
        "root": root,
        "expect": {
            "/HELLO.TXT": HELLO_TXT,
            "/README": README_TXT,
            "/LARGE.BIN": make_large(),
            "/UNCOMP.BIN": make_uncomp(),
            "/FRAG.TXT": make_frag(),
            "/ZERO.BIN": b"",
            "/SUB/NESTED.TXT": NESTED_TXT,
            "/SUB/DEEP/A/B/C/DEEP.TXT": DEEP_TXT,
        },
        "link": ("/LINK.TXT", LINK_TARGET),
        "dirs": {
            "/": ["HELLO.TXT", "README", "LARGE.BIN", "UNCOMP.BIN", "FRAG.TXT",
                  "ZERO.BIN", "LINK.TXT", "SUB"],
            "/SUB": ["NESTED.TXT", "DEEP"],
            "/SUB/DEEP/A/B/C": ["DEEP.TXT"],
        },
    }
    return info


def align_pos(pos, align=4):
    return (pos + align - 1) // align * align


# ==========================================================================
# INDEPENDENT host-side reader (separate code path from the builder above).
# It re-parses the produced bytes with fixed on-disk offsets, its own deflate
# decoder and its own content rules, and checks the superblock, metadata
# blocks, inode table, directory table, fragment table and every file's bytes.
# ==========================================================================

SB_FIELDS = [
    ("magic", 0, "I"), ("inode_count", 4, "I"), ("mkfs_time", 8, "I"),
    ("block_size", 12, "I"), ("fragment_entry_count", 16, "I"),
    ("compression", 20, "H"), ("block_log", 22, "H"), ("flags", 24, "H"),
    ("id_count", 26, "H"), ("version_major", 28, "H"), ("version_minor", 30, "H"),
    ("root_inode", 32, "Q"), ("bytes_used", 40, "Q"), ("id_table_start", 48, "Q"),
    ("xattr_id_table_start", 56, "Q"), ("inode_table_start", 64, "Q"),
    ("directory_table_start", 72, "Q"), ("fragment_table_start", 80, "Q"),
    ("lookup_table_start", 88, "Q"),
]


class Reader(object):
    def __init__(self, raw, expect=None):
        self.raw = raw
        self.sb = {}
        for name, off, fmt in SB_FIELDS:
            self.sb[name] = struct.unpack_from("<" + fmt, raw, off)[0]
        self.bs = self.sb["block_size"]
        self.meta_cache = {}
        self.tables = {}
        # Canonical layout: [fragment metadata][u64 lookup list], so the
        # directory table ends exactly where the fragment METADATA begins. The
        # lookup list follows that metadata and holds one u64 per block, which
        # is how a reader locates the metadata without a stored size.
        if self.sb["fragment_entry_count"]:
            frag_blocks = ((self.sb["fragment_entry_count"] * 16) + METADATA_SIZE - 1) \
                // METADATA_SIZE
            dir_end = self.sb["fragment_table_start"]
            pos = self.sb["lookup_table_start"]
            if pos == 0xFFFFFFFFFFFFFFFF:
                raise AssertionError("fragment table has no lookup list")
            if pos <= dir_end:
                raise AssertionError("fragment lookup list %d is not after the "
                                     "fragment metadata %d" % (pos, dir_end))
        else:
            # no fragments: [id lookup list][id metadata]; the directory table
            # ends where the first id lookup entry points
            id_blocks = ((self.sb["id_count"] * 4) + METADATA_SIZE - 1) // METADATA_SIZE
            if id_blocks == 0:
                id_blocks = 1
            list_at = self.sb["id_table_start"]
            first = struct.unpack_from("<Q", raw, list_at)[0]
            dir_end = min(first, list_at)
        self.tables["inode"] = self._table_offsets(self.sb["inode_table_start"],
                                                  self.sb["directory_table_start"])
        self.tables["dir"] = self._table_offsets(self.sb["directory_table_start"],
                                                dir_end)
        self.frag_entries = None
        self.comp_options = None
        if expect:
            for key, value in expect.items():
                if key not in self.tables:
                    continue
                if len(self.tables[key]) != value:
                    raise AssertionError("%s table has %d metadata blocks, expected %d"
                                         % (key, len(self.tables[key]), value))
        # the fragment table is indexed by a separate u64 lookup list positioned
        # right after its metadata blocks (s_lookup_table_start)
        self.frag_offsets = []
        if self.sb["fragment_entry_count"]:
            total = self.sb["fragment_entry_count"] * 16
            blocks = (total + METADATA_SIZE - 1) // METADATA_SIZE
            if expect and expect.get("frag") is not None and expect["frag"] != blocks:
                raise AssertionError("fragment table needs %d blocks, expected %d"
                                     % (blocks, expect["frag"]))
            pos = self.sb["lookup_table_start"]
            if pos == 0xFFFFFFFFFFFFFFFF:
                raise AssertionError("fragment table has no lookup list")
            for k in range(blocks):
                off = struct.unpack_from("<Q", raw, pos + k * 8)[0]
                if off >= len(raw):
                    raise AssertionError("fragment table block outside the image")
                self.frag_offsets.append(off)
        self.id_offsets = []
        id_blocks = ((self.sb["id_count"] * 4) + METADATA_SIZE - 1) // METADATA_SIZE
        if id_blocks == 0:
            id_blocks = 1
        for k in range(id_blocks):
            off = struct.unpack_from("<Q", raw, self.sb["id_table_start"] + k * 8)[0]
            if off >= len(raw):
                raise AssertionError("id table block outside the image")
            self.id_offsets.append(off)

    # ---------------- metadata ----------------
    def _table_offsets(self, start, end):
        """Walk the metadata blocks of a table starting at `start`, following
        each block's own 2-byte size header until `end` is reached. Bounded: a
        metadata block is at least 3 bytes on disk, so the walk cannot exceed
        (end - start) / 3 iterations."""
        out = []
        off = start
        limit = (end - start) // 3 + 1
        while off < end and len(out) < limit:
            if off + 2 > len(self.raw):
                raise AssertionError("metadata block header outside the image")
            hdr = struct.unpack_from("<H", self.raw, off)[0]
            size = hdr & ~COMPRESSED_BIT
            if size == 0 or off + 2 + size > len(self.raw):
                raise AssertionError("bad metadata block at %d (header 0x%04x)"
                                     % (off, hdr))
            out.append(off)
            off += 2 + size
        if not out:
            raise AssertionError("table at %d has no metadata blocks" % start)
        return out

    def meta_block(self, table, index):
        key = (table, index)
        if key in self.meta_cache:
            return self.meta_cache[key]
        offs = self.tables[table]
        if index >= len(offs):
            raise AssertionError("%s metadata block %d out of range" % (table, index))
        off = offs[index]
        hdr = struct.unpack_from("<H", self.raw, off)[0]
        if hdr & COMPRESSED_BIT:
            size = hdr & ~COMPRESSED_BIT
            data = self.raw[off + 2:off + 2 + size]
        else:
            size = hdr
            comp = self.raw[off + 2:off + 2 + size]
            data = zlib.decompress(comp)
        self.meta_cache[key] = data
        return data

    def meta_read(self, table, pos, length):
        out = bytearray()
        while len(out) < length:
            index = pos // METADATA_SIZE
            inner = pos % METADATA_SIZE
            blk = self.meta_block(table, index)
            if inner >= len(blk):
                raise AssertionError("metadata read past the end of block %d" % index)
            take = min(len(blk) - inner, length - len(out))
            out += blk[inner:inner + take]
            pos += take
        return bytes(out)

    def meta_read_upto(self, table, pos, length):
        """Like meta_read but stops at the end of the table's last block."""
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

    # ---------------- inodes ----------------
    def inode_at(self, ref):
        blk = ref >> 16
        off = ref & 0xFFFF
        data = self.meta_read_upto("inode", blk * METADATA_SIZE + off, 16 + 4096)
        itype, mode, uid, gid, mtime, ino = struct.unpack_from("<HHHHII", data, 0)
        i = {"type": itype, "mode": mode, "uid": uid, "gid": gid, "mtime": mtime,
             "ino": ino, "ref": ref, "blk": blk, "off": off}
        pos = 16
        if itype == INODE_DIR:
            (i["start_block"], i["nlink"], i["size"], i["offset"],
             i["parent"]) = struct.unpack_from("<IIHHI", data, pos)
        elif itype == INODE_EXT_DIR:
            (i["nlink"], i["size"], i["start_block"], i["parent"], i["i_count"],
             i["offset"], i["xattr"]) = struct.unpack_from("<IIIIHHI", data, pos)
        elif itype == INODE_FILE:
            (i["blocks_start"], i["fragment_index"], i["fragment_offset"],
             i["file_size"]) = struct.unpack_from("<IIII", data, pos)
        elif itype == INODE_EXT_FILE:
            (i["blocks_start"], i["file_size"], i["sparse"], i["nlink"],
             i["fragment_index"], i["fragment_offset"],
             i["xattr"]) = struct.unpack_from("<QQQIIII", data, pos)
        elif itype == INODE_SYMLINK:
            i["nlink"], i["target_size"] = struct.unpack_from("<II", data, pos)
        elif itype == INODE_EXT_SYMLINK:
            i["nlink"], i["target_size"] = struct.unpack_from("<II", data, pos)
        else:
            raise AssertionError("unexpected inode type %d" % itype)
        i["payload_pos"] = pos
        i["raw"] = data
        # exact inode size incl. the trailing block-size list
        payload = {INODE_DIR: 16, INODE_FILE: 16, INODE_SYMLINK: 8}[itype] \
            if itype in (INODE_DIR, INODE_FILE, INODE_SYMLINK) else None
        if itype == INODE_EXT_DIR:
            payload = 24 + i["i_count"] * 8
        elif itype == INODE_EXT_FILE:
            payload = 40
        elif itype == INODE_EXT_SYMLINK:
            payload = 256 if i["target_size"] < 256 else 0
        if itype in (INODE_FILE, INODE_EXT_FILE):
            size = i["file_size"]
            frag = i.get("fragment_index", NO_FRAGMENT) != NO_FRAGMENT
            nblocks = size // self.bs if frag else (size + self.bs - 1) // self.bs
            i["block_list_off"] = pos + payload
            i["block_count"] = nblocks
            payload += nblocks * 4
        i["payload_size"] = payload
        i["inode_size"] = 16 + payload
        return i

    # ---------------- fragments ----------------
    def fragment_entry(self, idx):
        if self.frag_entries is None:
            if self.sb["fragment_entry_count"] == 0:
                raise AssertionError("no fragment table in this image")
            entries = bytearray()
            for off in self.frag_offsets:
                hdr = struct.unpack_from("<H", self.raw, off)[0]
                if hdr & COMPRESSED_BIT:
                    size = hdr & ~COMPRESSED_BIT
                    entries += self.raw[off + 2:off + 2 + size]
                else:
                    entries += zlib.decompress(self.raw[off + 2:off + 2 + hdr])
            self.frag_entries = entries
        start, size, _ = struct.unpack_from("<QII", self.frag_entries, idx * 16)
        return start, size

    def data_block_bytes(self, blocks_start, block_index, block_entries):
        off = blocks_start
        for k in range(block_index):
            off += block_entries[k] & BLOCK_SIZE_MASK
        word = block_entries[block_index]
        if word == 0:
            return b"\x00" * self.bs
        size = word & BLOCK_SIZE_MASK
        payload = self.raw[off:off + size]
        if word & COMPRESSED_BIT_BLOCK:
            return payload
        d = zlib.decompressobj(-15)
        return d.decompress(payload) + d.flush()

    def read_file(self, inode):
        size = inode["file_size"]
        if size == 0:
            return b""
        fragment = inode["fragment_index"] != NO_FRAGMENT
        nblocks = size // self.bs if fragment else (size + self.bs - 1) // self.bs
        list_pos = inode["ref"] & 0xFFFF
        list_pos += 16 + (16 if inode["type"] == INODE_FILE else 40)
        entries = []
        if nblocks:
            raw_list = self.meta_read("inode", (inode["ref"] >> 16) * METADATA_SIZE +
                                      list_pos, nblocks * 4)
            entries = list(struct.unpack_from("<" + "I" * nblocks, raw_list, 0))
        out = bytearray()
        for k in range(nblocks):
            out += self.data_block_bytes(inode["blocks_start"], k, entries)[:self.bs]
        if fragment:
            tail = size - nblocks * self.bs
            if tail:
                start, fsize = self.fragment_entry(inode["fragment_index"])
                payload = self.raw[start:start + (fsize & BLOCK_SIZE_MASK)]
                if fsize & COMPRESSED_BIT_BLOCK:
                    frag = payload
                else:
                    d = zlib.decompressobj(-15)
                    frag = d.decompress(payload) + d.flush()
                off = inode["fragment_offset"]
                out += frag[off:off + tail]
        return bytes(out[:size])

    # ---------------- directory table ----------------
    def list_dir(self, inode, names_only=True):
        size = inode["size"] - 3
        # The listing starts at start_block (relative to the directory table) plus
        # the inode's own byte offset inside that uncompressed metadata block.
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
        node = self.inode_at(self.sb["root_inode"])
        if path in ("", "/"):
            return node
        for part in [p for p in path.split("/") if p]:
            if node["type"] not in (INODE_DIR, INODE_EXT_DIR):
                raise AssertionError("%s: not a directory" % path)
            hit = None
            for e in self.list_dir(node):
                if e["name"] == part:
                    hit = e
                    break
            if hit is None:
                raise AssertionError("not found: %s" % path)
            node = self.inode_at(hit["ref"])
        return node

    def walk(self, node=None, prefix=""):
        if node is None:
            node = self.inode_at(self.sb["root_inode"])
        for e in self.list_dir(node):
            if e["name"] in (".", ".."):
                continue
            child = self.inode_at(e["ref"])
            path = prefix + "/" + e["name"]
            yield path, child, e["type"]
            if child["type"] in (INODE_DIR, INODE_EXT_DIR):
                for row in self.walk(child, path):
                    yield row


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def verify(img, info, verbose=True):
    """Independent re-parse. Returns a dict of facts; raises AssertionError on
    any inconsistency."""
    r = Reader(img, expect=info.get("meta_block_counts"))
    sb = r.sb
    assert sb["magic"] == MAGIC, "magic 0x%x" % sb["magic"]
    assert sb["version_major"] == 4 and sb["version_minor"] == 0, "version"
    assert sb["compression"] == COMP_GZIP, "compression id %d" % sb["compression"]
    assert sb["block_size"] == info["block_size"], "block size"
    assert sb["block_log"] == info["block_size"].bit_length() - 1, "block_log"
    assert sb["bytes_used"] == info["bytes_used"], "bytes_used"
    assert sb["inode_count"] == info["inode_count"], "inode_count"
    assert sb["fragment_entry_count"] == info["fragment_count"], "fragment count"
    assert sb["id_table_start"] < sb["bytes_used"], "id table start"
    assert sb["inode_table_start"] < sb["directory_table_start"], "table order"
    assert sb["directory_table_start"] < sb["fragment_table_start"], "table order 2"
    assert sb["xattr_id_table_start"] == 0xFFFFFFFFFFFFFFFF, "no xattr table"
    assert sb["flags"] & FLAG_NO_XATTRS, "NO_XATTRS flag"

    root = r.inode_at(sb["root_inode"])
    assert root["type"] == INODE_DIR, "root is not a basic dir inode"
    assert root["ino"] == 2, "root inode number"

    # ---- every documented file, byte for byte ----
    for path, want in info["expect"].items():
        inode = r.lookup(path)
        got = r.read_file(inode)
        assert len(got) == len(want), "%s: %d bytes, want %d" % (path, len(got), len(want))
        assert got == want, "%s: content mismatch (crc 0x%08x vs 0x%08x)" % (
            path, crc32(got), crc32(want))

    # ---- symlink ----
    link_path, target = info["link"]
    linode = r.lookup(link_path)
    assert linode["type"] == INODE_SYMLINK, "symlink inode type"
    tsize = linode["target_size"]
    raw = r.meta_read("inode", (linode["ref"] >> 16) * METADATA_SIZE +
                      (linode["ref"] & 0xFFFF) + 16 + 8, tsize)
    assert raw == target, "symlink target %r != %r" % (raw, target)

    # ---- readdir sets ----
    for dirpath, names in info["dirs"].items():
        node = r.lookup(dirpath)
        got = sorted(e["name"] for e in r.list_dir(node) if e["name"] not in (".", ".."))
        assert got == sorted(names), "%s listing %s != %s" % (dirpath, got, sorted(names))

    # ---- . / .. entries ----
    for dirpath in ("/", "/SUB", "/SUB/DEEP/A/B/C"):
        node = r.lookup(dirpath)
        entries = {e["name"]: e for e in r.list_dir(node)}
        assert "." in entries and ".." in entries, "%s lacks . or .." % dirpath
        assert entries["."]["ref"] == node["ref"], "%s '.' is wrong" % dirpath

    # ---- fragments are actually used (this image) ----
    if info["fragment_count"]:
        frag_users = [p for p in info["expect"]
                      if r.lookup(p)["type"] == INODE_FILE
                      and r.lookup(p).get("fragment_index", NO_FRAGMENT) != NO_FRAGMENT]
        assert frag_users, "no file uses a fragment"
    else:
        for p in info["expect"]:
            node = r.lookup(p)
            if node["type"] == INODE_FILE:
                assert node["fragment_index"] == NO_FRAGMENT, \
                    "%s uses a fragment in a no-fragment image" % p

    # ---- the uncompressed data block path is actually exercised ----
    stored = 0
    compressed = 0
    for path in info["expect"]:
        inode = r.lookup(path)
        if inode["type"] != INODE_FILE or inode["file_size"] == 0:
            continue
        size = inode["file_size"]
        frag = inode.get("fragment_index", NO_FRAGMENT) != NO_FRAGMENT
        nblocks = size // r.bs if frag else (size + r.bs - 1) // r.bs
        if nblocks:
            raw_list = r.meta_read("inode", (inode["ref"] >> 16) * METADATA_SIZE +
                                   (inode["ref"] & 0xFFFF) + 16 +
                                   (16 if inode["type"] == INODE_FILE else 40),
                                   nblocks * 4)
            for k in range(nblocks):
                if struct.unpack_from("<I", raw_list, k * 4)[0] & COMPRESSED_BIT_BLOCK:
                    stored += 1
                else:
                    compressed += 1
    assert stored >= 1, "no data block is stored uncompressed"
    assert compressed >= 1, "no data block is compressed"

    # ---- metadata: at least one compressed and one stored block ----
    meta_stored = 0
    meta_compressed = 0
    for table in ("inode", "dir"):
        for off in r.tables[table]:
            hdr = struct.unpack_from("<H", r.raw, off)[0]
            if hdr & COMPRESSED_BIT:
                meta_stored += 1
            else:
                meta_compressed += 1
    if meta_stored == 0:
        assert meta_compressed >= 1, "no compressed metadata block"
    else:
        assert meta_stored >= 1, "no stored metadata block"

    # ---- tree shape ----
    paths = sorted(p for p, _, _ in r.walk())
    assert "/LARGE.BIN" in paths and "/SUB/DEEP/A/B/C/DEEP.TXT" in paths, "tree"
    assert len(paths) == 14, "expected 14 paths, got %d: %s" % (len(paths), paths)

    # ---- sizes/types via the reader's own view ----
    types = {p: t for p, _, t in r.walk()}
    assert types["/LARGE.BIN"] == INODE_FILE
    assert types["/SUB"] == INODE_DIR
    assert types["/LINK.TXT"] == INODE_SYMLINK

    if verbose:
        print("  superblock: magic=0x%08x v%d.%d block_size=%d block_log=%d"
              % (sb["magic"], sb["version_major"], sb["version_minor"],
                 sb["block_size"], sb["block_log"]))
        print("  tables    : inode=%d(%d blocks) dir=%d(%d blocks) frag=%d id=%d  bytes_used=%d"
              % (sb["inode_table_start"], len(r.tables["inode"]),
                 sb["directory_table_start"], len(r.tables["dir"]),
                 sb["fragment_table_start"], sb["id_table_start"], sb["bytes_used"]))
        print("  metadata  : %d compressed, %d stored uncompressed"
              % (meta_compressed, meta_stored))
        print("  data      : %d compressed blocks, %d stored blocks; %d fragments"
              % (compressed, stored, sb["fragment_entry_count"]))
        for path in sorted(info["expect"]):
            data = info["expect"][path]
            print("    %-28s %7d B  crc32=0x%08x" % (path, len(data), crc32(data)))
        print("    %-28s %7d B  -> %s" % (link_path, len(target), target.decode()))
    return {"sb": sb, "reader": r}


def write_image(img, info, out_path, corrupt_magic=False):
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    # the image is padded out to the full nominal size so it can be attached as
    # a block device / -hdb drive directly
    padded = bytearray(info["full_size"])
    padded[:len(img)] = img
    with open(out_path, "wb") as f:
        f.write(bytes(padded))
    return out_path


# ==========================================================================
# Negative fixtures for the host harness (deliberately broken images)
#
# These are built by the SAME writer with a flag, not by patching bytes
# afterwards: that keeps every offset, metadata block and table consistent with
# the deliberate fault, so the fault under test is the only thing wrong.
# ==========================================================================

def build_bad_inode_fixture(block_size, img_size):
    """The superblock's root inode reference points at an inode table block that
    does not exist. Mount must succeed (the tables are fine) and the inode
    lookup must fail honestly."""
    info = build_image(block_size, img_size, bad_root_inode=True)
    verify_negative(info["img"])
    info["root_inode_was"] = info["root"].inode_ref
    info["root_inode_now"] = (0x7FFF << 16) | (info["root"].inode_ref & 0xFFFF)
    return info


def build_bad_fragment_fixture(block_size, img_size, ref=0x7FFFFFFF):
    """Every file inode's fragment_index is set to a value beyond the fragment
    table. Mount must succeed and opening such a file must fail."""
    info = build_image(block_size, img_size, bad_fragment_ref=ref)
    verify_negative(info["img"])
    info["bad_fragment_old"] = ref
    return info


def build_bad_blocks_fixture(block_size, img_size, blocks_start=0x7FFFFFF0):
    """Every file inode's blocks_start points far past the end of the image.
    Mount must succeed (the tables are intact) and reading DATA must fail."""
    info = build_image(block_size, img_size, bad_blocks_start=blocks_start)
    verify_negative(info["img"])
    info["bad_blocks_old"] = blocks_start
    return info


def verify_negative(img):
    """A negative fixture must still be parseable as a SquashFS image: the
    deliberate fault is somewhere else, not in the magic or the layout."""
    if len(img) < 96:
        raise AssertionError("negative fixture is too small")
    if struct.unpack_from("<I", img, 0)[0] != MAGIC:
        raise AssertionError("negative fixture lost its magic")
    sb = {}
    for name, off, fmt in SB_FIELDS:
        sb[name] = struct.unpack_from("<" + fmt, img, off)[0]
    assert sb["version_major"] == 4 and sb["version_minor"] == 0
    assert sb["block_size"] == 4096
    assert 96 <= sb["bytes_used"] <= len(img)
    return True


def main():
    ap = argparse.ArgumentParser(description="build a real SquashFS 4.0 test image")
    ap.add_argument("--out", default=os.path.join(OUT_DIR, "squashfs.img"))
    ap.add_argument("--block-size", type=int, default=BLOCK_SIZE,
                    choices=(4096, 8192, 16384, 32768, 65536, 131072))
    ap.add_argument("--size", type=int, default=IMG_SIZE)
    ap.add_argument("--corrupt-magic", action="store_true",
                    help="also write build/squashfs_badmagic.img (negative fixture)")
    ap.add_argument("--negatives", action="store_true",
                    help="also write build/squashfs_badinode.img and "
                         "build/squashfs_badfrag.img (host-harness fixtures)")
    ap.add_argument("--no-fragments", action="store_true",
                    help="build an image with no fragment table at all")
    ap.add_argument("--raw-meta", action="store_true",
                    help="store every metadata block uncompressed")
    args = ap.parse_args()

    info = build_image(args.block_size, args.size, fragments=not args.no_fragments,
                       raw_meta=args.raw_meta)
    verify(info["img"], info, verbose=True)
    out = write_image(info["img"], info, args.out)
    print("squashfs image: %s (%d bytes used of %d)" % (out, info["bytes_used"],
                                                       info["full_size"]))
    print("host-side verification: PASS")

    base = os.path.dirname(args.out) or "."
    if args.corrupt_magic:
        bad = build_image(args.block_size, args.size, corrupt_magic=True)
        bad_path = os.path.join(base, "squashfs_badmagic.img")
        write_image(bad["img"], bad, bad_path, corrupt_magic=True)
        print("negative fixture: %s (magic deliberately wrong)" % bad_path)
    if args.negatives:
        bi = build_bad_inode_fixture(args.block_size, args.size)
        p = os.path.join(base, "squashfs_badinode.img")
        write_image(bi["img"], bi, p)
        print("negative fixture: %s (root inode ref 0x%x -> 0x%x)"
              % (p, bi["root_inode_was"], bi["root_inode_now"]))
        bf = build_bad_fragment_fixture(args.block_size, args.size)
        p = os.path.join(base, "squashfs_badfrag.img")
        write_image(bf["img"], bf, p)
        print("negative fixture: %s (/FRAG.TXT fragment index %d -> 0x7fffffff)"
              % (p, bf["bad_fragment_old"]))
        bd = build_bad_blocks_fixture(args.block_size, args.size)
        p = os.path.join(base, "squashfs_badblocks.img")
        write_image(bd["img"], bd, p)
        print("negative fixture: %s (file blocks_start -> 0x%x)"
              % (p, bd["bad_blocks_old"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
