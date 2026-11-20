#!/usr/bin/env python3
"""SutureOS: host-side verifier for a VimtuFS2 image (independent parser).

Purpose: prove, from the host and without any kernel code, that an image that a
SutureOS guest just mounted is a *valid* VimtuFS2 volume AND that the guest did
not modify a single byte of it (the driver is read-only).

The parser here is written from scratch on purpose - it does **not** import
scripts/mkfs_vimtufs2_testimg.py, so a bug in the generator cannot hide behind
the same bug in the verifier.  Every check re-derives the value from the raw
bytes:

  image      size + SHA-256 against build/vimtufs2_manifest.json
  superblock magic "VIMTUFS2", 0xAA55, CRC32 over [0,60), version, geometry
             fully recomputed (bitmap/inode/data layout must add up exactly),
             feature extension header must be zero (feature-free volume)
  bitmap     the set of used data blocks must equal the set the inode tree
             references (no orphans, no double allocation), free count matches
             the manifest
  inodes     CRC32 over [0,124), type/name/kind/nlink/mode/timestamp field
             rules, directories carry no data blocks, file size limits, the
             indirect blocks a file needs must exist
  tree       every inode's parent is a live directory, the parent chain reaches
             the root, the root points at itself with an empty name, directory
             link counts are 1 + direct subdirectories
  contents   every file's bytes are read back through d0..d3/ind/dind and
             hashed; the huge file's block chain must really cover the size
             (this is what proves the double indirect mapping is correct)

Format spec: VimtuOS kernel/vfs64.h + kernel/vfs64.cpp (GPL-3.0); this verifier
is an independent re-implementation written from that spec.

Usage:
  py -3 scripts/verify_vimtufs2_read.py build/vimtufs2.img
  py -3 scripts/verify_vimtufs2_read.py build/vimtufs2_v4.img \
        --manifest build/vimtufs2_v4_manifest.json
Exit code 0 = PASS, 1 = at least one check failed.
"""
import argparse
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ---- format constants (VimtuOS vfs64.h:166-204) ----
SECTOR = 512
INODE_BYTES = 128
INODES_PER_BLOCK = 4
NAME_MAX = 31
DIRECT = 4
IND_PTRS = 128
DIND_CHILDREN = 128
L2_FIRST = DIRECT + IND_PTRS                 # 132
MAX_MAP = L2_FIRST + DIND_CHILDREN * IND_PTRS
MAX_FILE_BLOCKS = 16384
MIN_BLOCKS = 32
MAX_INODES = 2048

# superblock offsets (vfs64.cpp:39-65)
SB_MAGIC, SB_VERSION, SB_SECTOR, SB_BLOCK, SB_TOTAL = 0, 8, 12, 16, 20
SB_ROOT, SB_BITMAP, SB_BITMAPN, SB_INODE, SB_INODEN = 24, 28, 32, 36, 40
SB_INOSZ, SB_DATA, SB_DATAN, SB_FLAGS, SB_CRC = 44, 48, 52, 56, 60
SB_FEATBITS, SB_FEATCRC, SB_SIG = 64, 84, 510

# inode offsets (vfs64.cpp:69-97)
I_TYPE, I_NAMELEN, I_RSVD, I_SIZE = 0, 1, 2, 4
I_D0, I_IND, I_PARENT, I_MTIME, I_NLINK = 8, 24, 28, 32, 36
I_KIND, I_RSVD2, I_NAME, I_DIND = 38, 39, 40, 71
I_UID, I_GID, I_MODE, I_XATTR, I_SNAPF, I_CRC = 75, 77, 79, 81, 85, 124

T_FREE, T_FILE, T_DIR, T_LINK = 0, 1, 2, 3
KIND_MAX = 6
S_IFMT, S_IFREG, S_IFDIR, S_IFLNK = 0o170000, 0o100000, 0o040000, 0o120000
LEGACY_RSVD = {3: 75, 4: 81, 5: 89}


def u16(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


class Verifier:
    def __init__(self):
        self.checks = []
        self.fails = []

    def ok(self, name, cond, detail=""):
        cond = bool(cond)
        self.checks.append((name, cond, detail))
        if not cond:
            self.fails.append(name)
        return cond

    def report(self):
        for name, good, detail in self.checks:
            line = "  [%s] %s" % ("ok" if good else "FAIL", name)
            if not good and detail:
                line += "  (" + str(detail) + ")"
            print(line)
        ok = len(self.checks) - len(self.fails)
        print("[vimtufs2-verify] checks=%d ok=%d fail=%d"
              % (len(self.checks), ok, len(self.fails)))
        print("[vimtufs2-verify] RESULT:", "PASS" if not self.fails else "FAIL")
        return 0 if not self.fails else 1


def geometry(total):
    bitmap_blocks = (total + SECTOR * 8 - 1) // (SECTOR * 8)
    inodes = total // 16
    inodes = max(16, min(inodes, MAX_INODES))
    inode_blocks = (inodes + INODES_PER_BLOCK - 1) // INODES_PER_BLOCK
    return {
        "blocks": total,
        "bitmap_start": 1,
        "bitmap_blocks": bitmap_blocks,
        "inode_start": 1 + bitmap_blocks,
        "inode_blocks": inode_blocks,
        "inodes": inodes,
        "data_start": 1 + bitmap_blocks + inode_blocks,
        "data_blocks": total - (1 + bitmap_blocks + inode_blocks),
    }


def block(img, idx):
    return img[idx * SECTOR:(idx + 1) * SECTOR]


def inode_at(img, g, idx):
    blk = g["inode_start"] + idx // INODES_PER_BLOCK
    off = (idx % INODES_PER_BLOCK) * INODE_BYTES
    return img[blk * SECTOR + off:blk * SECTOR + off + INODE_BYTES]


def mapped_blocks(img, g, raw, version):
    """Walk d0..d3 -> ind -> dind and return the volume block of every fs block.

    A 0 entry means "not mapped", which inside the file size is corruption.
    """
    nblk = (u32(raw, I_SIZE) + SECTOR - 1) // SECTOR
    out = [u32(raw, I_D0 + 4 * i) for i in range(DIRECT)]
    ind = u32(raw, I_IND)
    if ind:
        table = block(img, ind)
        out += [u32(table, 4 * i) for i in range(IND_PTRS)]
    else:
        out += [0] * IND_PTRS
    dind = u32(raw, I_DIND) if version >= 3 else 0
    if dind:
        table = block(img, dind)
        for c in range(DIND_CHILDREN):
            child = u32(table, 4 * c)
            if child:
                ctable = block(img, child)
                out += [u32(ctable, 4 * i) for i in range(IND_PTRS)]
            else:
                out += [0] * IND_PTRS
    else:
        out += [0] * (DIND_CHILDREN * IND_PTRS)
    if nblk > len(out):
        return out, nblk          # size needs more blocks than can ever be mapped
    return out[:nblk], nblk


def verify(image_path, manifest_path):
    v = Verifier()
    image_path = Path(image_path)
    manifest = json.loads(Path(manifest_path).read_text(encoding="utf-8"))
    img = image_path.read_bytes()
    rec = {f["name"]: f for f in manifest["files"]}
    version_expected = manifest["version"]

    # ---------------- 1. what the guest must not have touched -------------
    v.ok("image size is %d bytes (manifest)" % manifest["image"]["size"],
         len(img) == manifest["image"]["size"], "got %d" % len(img))
    digest = hashlib.sha256(img).hexdigest()
    v.ok("image sha256 is unchanged from the manifest (guest wrote nothing)",
         digest == manifest["image"]["sha256"],
         "%s != %s" % (digest, manifest["image"]["sha256"]))

    # ---------------- 2. superblock ---------------------------------------
    sb = block(img, 0)
    v.ok("superblock magic is VIMTUFS2", sb[:8] == b"VIMTUFS2", repr(bytes(sb[:8])))
    v.ok("superblock CRC32 over [0,60) is valid",
         u32(sb, SB_CRC) == crc32(bytes(sb[:SB_CRC])),
         "%08x != %08x" % (u32(sb, SB_CRC), crc32(bytes(sb[:SB_CRC]))))
    v.ok("superblock ends with 0xAA55", u16(sb, SB_SIG) == 0xAA55,
         "%04x" % u16(sb, SB_SIG))
    version = u32(sb, SB_VERSION)
    v.ok("version is %d as recorded" % version_expected, version == version_expected,
         "got %d" % version)
    v.ok("version is one of 3/4/5", version in (3, 4, 5))
    v.ok("sector size and block size are 512",
         u32(sb, SB_SECTOR) == SECTOR and u32(sb, SB_BLOCK) == SECTOR)
    v.ok("inode record size is 128", u32(sb, SB_INOSZ) == INODE_BYTES)

    total = u32(sb, SB_TOTAL)
    v.ok("total block count matches the file size",
         total == len(img) // SECTOR, "%d vs %d" % (total, len(img) // SECTOR))
    g = geometry(total)
    v.ok("bitmap starts right after the superblock", u32(sb, SB_BITMAP) == g["bitmap_start"])
    v.ok("bitmap block count recomputed", u32(sb, SB_BITMAPN) == g["bitmap_blocks"])
    v.ok("inode table starts right after the bitmap",
         u32(sb, SB_INODE) == g["inode_start"])
    v.ok("inode count is total/16 clamped to [16,2048]",
         u32(sb, SB_INODEN) == g["inodes"],
         "%d vs %d" % (u32(sb, SB_INODEN), g["inodes"]))
    v.ok("data area starts right after the inode table",
         u32(sb, SB_DATA) == g["data_start"])
    v.ok("data block count recomputed", u32(sb, SB_DATAN) == g["data_blocks"])
    v.ok("root inode number is 0", u32(sb, SB_ROOT) == 0)
    v.ok("reserved superblock field is 0", u32(sb, SB_FLAGS) == 0)
    v.ok("volume is big enough", total >= MIN_BLOCKS)

    feat = u32(sb, SB_FEATBITS)
    v.ok("feature bits are 0 (a feature-free v%s volume)" % version, feat == 0,
         "0x%x" % feat)
    v.ok("feature extension header [64,84) is all zero",
         bytes(sb[SB_FEATBITS:SB_FEATCRC]) == b"\0" * 20)
    v.ok("superblock bytes [88,510) are zero",
         bytes(sb[88:SB_SIG]) == b"\0" * (SB_SIG - 88))

    # ---------------- 3. inode table structure ----------------------------
    rsvd_from = LEGACY_RSVD[version]
    live = []
    for idx in range(g["inodes"]):
        raw = inode_at(img, g, idx)
        t = raw[I_TYPE]
        if t == T_FREE:
            continue
        live.append(idx)
        where = "inode %d" % idx
        v.ok(where + ": type is 1/2/3", t in (T_FILE, T_DIR, T_LINK), t)
        v.ok(where + ": namelen <= 31", raw[I_NAMELEN] <= NAME_MAX, raw[I_NAMELEN])
        v.ok(where + ": u16 reserved is 0", u16(raw, I_RSVD) == 0)
        v.ok(where + ": rsvd2 is 0", raw[I_RSVD2] == 0)
        v.ok(where + ": CRC32 over [0,124)",
             u32(raw, I_CRC) == crc32(bytes(raw[:I_CRC])),
             "%08x" % u32(raw, I_CRC))
        v.ok(where + ": kind <= 6", raw[I_KIND] <= KIND_MAX, raw[I_KIND])
        v.ok(where + ": nlink != 0", u16(raw, I_NLINK) != 0)
        v.ok(where + ": reserved bytes are zero",
             bytes(raw[rsvd_from:I_CRC]) == b"\0" * (I_CRC - rsvd_from))
        if version >= 4:
            mode = u16(raw, I_MODE)
            want = S_IFDIR if t == T_DIR else (S_IFLNK if t == T_LINK else S_IFREG)
            v.ok(where + ": mode type bits match the inode type",
                 (mode & S_IFMT) == want, "0o%o" % mode)
            v.ok(where + ": mode bits stay inside 0777",
                 (mode & ~(S_IFMT | 0o777)) == 0, "0o%o" % mode)
        mt = u32(raw, I_MTIME)
        if mt:
            v.ok(where + ": packed mtime fields in range",
                 1 <= ((mt >> 22) & 0xF) <= 12 and 1 <= ((mt >> 17) & 0x1F) <= 31 and
                 ((mt >> 12) & 0x1F) <= 23 and ((mt >> 6) & 0x3F) <= 59 and
                 (mt & 0x3F) <= 59, hex(mt))
        if t == T_DIR:
            v.ok(where + ": directory has size 0 and no data/indirect blocks",
                 u32(raw, I_SIZE) == 0 and
                 all(u32(raw, I_D0 + 4 * i) == 0 for i in range(DIRECT)) and
                 u32(raw, I_IND) == 0 and u32(raw, I_DIND) == 0)
        if t == T_FILE:
            size = u32(raw, I_SIZE)
            v.ok(where + ": file size <= 8 MiB", size <= MAX_FILE_BLOCKS * SECTOR)
            nblk = (size + SECTOR - 1) // SECTOR
            v.ok(where + ": indirect block present when needed",
                 nblk <= DIRECT or u32(raw, I_IND) != 0)
            v.ok(where + ": double indirect block present when needed",
                 nblk <= L2_FIRST or u32(raw, I_DIND) != 0)
            for label, b in (("ind", u32(raw, I_IND)), ("dind", u32(raw, I_DIND))):
                if b:
                    v.ok(where + ": %s block lives in the data area" % label,
                         g["data_start"] <= b < total, b)

    # ---------------- 4. block ownership: bitmap vs inode tree ------------
    referenced = set()
    problems = []

    def ref(blk, what):
        if not blk:
            return
        if not (g["data_start"] <= blk < total):
            problems.append("%s: block %d outside the data area" % (what, blk))
            return
        if blk in referenced:
            problems.append("%s: block %d referenced twice" % (what, blk))
        referenced.add(blk)

    file_chains = {}
    for idx in live:
        raw = inode_at(img, g, idx)
        if raw[I_TYPE] != T_FILE:
            continue
        mapped, nblk = mapped_blocks(img, g, raw, version)
        v.ok("inode %d: block map covers the file size (%d blocks)" % (idx, nblk),
             len(mapped) == nblk and all(mapped), "mapped=%d" % len(mapped))
        for i, b in enumerate(mapped):
            if not b:
                problems.append("inode %d: fs block %d unmapped inside the size" % (idx, i))
                continue
            ref(b, "inode %d fs block %d" % (idx, i))
        if u32(raw, I_IND):
            ref(u32(raw, I_IND), "inode %d ind" % idx)
        dind = u32(raw, I_DIND)
        if dind:
            ref(dind, "inode %d dind" % idx)
            table = block(img, dind)
            for c in range(DIND_CHILDREN):
                child = u32(table, 4 * c)
                if child:
                    ref(child, "inode %d dind child %d" % (idx, c))
        file_chains[idx] = mapped

    v.ok("no block is referenced twice and every reference is inside the data area",
         not problems, "; ".join(problems[:4]))
    bitmap = img[g["bitmap_start"] * SECTOR:
                 (g["bitmap_start"] + g["bitmap_blocks"]) * SECTOR]

    def used_bit(b):
        return (bitmap[b >> 3] >> (b & 7)) & 1

    v.ok("every metadata block (0..data_start-1) is marked used",
         all(used_bit(b) for b in range(g["data_start"])))
    v.ok("padding bits past the volume are marked used",
         all(used_bit(b) for b in range(total,
                                       g["bitmap_blocks"] * SECTOR * 8)))
    used = {b for b in range(g["data_start"], total) if used_bit(b)}
    v.ok("bitmap used set == the set the inode tree references",
         used == referenced,
         "bitmap-only=%s tree-only=%s" % (sorted(used - referenced)[:4],
                                          sorted(referenced - used)[:4]))
    v.ok("free block count (%d) matches the manifest (%d)"
         % (g["data_blocks"] - len(used), manifest["free_blocks"]),
         g["data_blocks"] - len(used) == manifest["free_blocks"])
    v.ok("manifest used-block list matches the bitmap",
         sorted(referenced) == sorted(manifest["used_data_blocks"]))

    # ---------------- 5. directory tree -----------------------------------
    parent, name, typ = {}, {}, {}
    for idx in live:
        raw = inode_at(img, g, idx)
        parent[idx] = u32(raw, I_PARENT)
        name[idx] = bytes(raw[I_NAME:I_NAME + raw[I_NAMELEN]]).decode("ascii", "replace")
        typ[idx] = raw[I_TYPE]
    root = u32(sb, SB_ROOT)
    v.ok("root inode 0 is a directory with no name pointing at itself",
         typ.get(root) == T_DIR and name.get(root) == "" and parent.get(root) == root)
    subdirs = sum(1 for i in live
                  if typ[i] == T_DIR and parent[i] == root and i != root)
    v.ok("root nlink == 1 + direct subdirectories",
         u16(inode_at(img, g, root), I_NLINK) == 1 + subdirs)
    for idx in live:
        if idx == root:
            continue
        if not v.ok("inode %d: parent is a live directory" % idx,
                    parent.get(idx) in typ and typ[parent[idx]] == T_DIR):
            continue
        seen, cur, good = set(), idx, True
        while cur != root:
            if cur in seen or cur not in parent:
                good = False
                break
            seen.add(cur)
            cur = parent[cur]
        v.ok("inode %d: parent chain reaches the root" % idx, good)

    # ---------------- 6. contents through the block tree ------------------
    for fname, r in rec.items():
        if r["type"] != "file":
            continue
        raw = inode_at(img, g, r["inode"])
        v.ok("%r: exists at inode %d with the recorded parent" % (fname, r["inode"]),
             name.get(r["inode"]) == r["name"] and parent.get(r["inode"]) == r["parent"])
        v.ok("%r: size as recorded" % fname, u32(raw, I_SIZE) == r["size"],
             "%d vs %d" % (u32(raw, I_SIZE), r["size"]))
        chain = file_chains.get(r["inode"], [])
        data = bytearray()
        for b in chain[:max(0, (r["size"] + SECTOR - 1) // SECTOR)]:
            data += block(img, b)
        data = bytes(data[:r["size"]])
        v.ok("%r: content sha256 as recorded" % fname,
             hashlib.sha256(data).hexdigest() == r["sha256"])
        v.ok("%r: block numbers as recorded" % fname,
             chain[:r["fs_blocks"]] == r["data_blocks"],
             "first difference at %s" % next(
                 (i for i, (a, b) in enumerate(zip(chain, r["data_blocks"])) if a != b),
                 "-"))
        if r["ind_block"]:
            ind = block(img, r["ind_block"])
            first = [u32(ind, 4 * i) for i in range(min(4, r["fs_blocks"] - DIRECT))]
            v.ok("%r: single indirect block maps fs block %d..%d" % (fname, DIRECT, DIRECT + 3),
                 first == r["data_blocks"][DIRECT:DIRECT + len(first)])
        if r["dind_block"]:
            dind = block(img, r["dind_block"])
            children = [u32(dind, 4 * c) for c in range(len(r["dind_child_blocks"]))]
            v.ok("%r: double indirect children as recorded" % fname,
                 children == r["dind_child_blocks"])
            for c, child in enumerate(r["dind_child_blocks"]):
                ctable = block(img, child)
                want = r["data_blocks"][L2_FIRST + c * IND_PTRS:
                                        L2_FIRST + (c + 1) * IND_PTRS]
                got = [u32(ctable, 4 * i) for i in range(len(want))]
                v.ok("%r: dind child %d maps its %d blocks" % (fname, c, len(want)),
                     got == want)

    # the fixture really has to exercise the indirect layers, otherwise this
    # verifier would pass on a volume that never touches them
    big = next((r for r in rec.values() if r["fs_blocks"] > L2_FIRST), None)
    v.ok("fixture contains a file that needs the double indirect block", big is not None)
    if big:
        v.ok("double-indirect file uses at least two child blocks",
             len(big["dind_child_blocks"]) >= 2)
        v.ok("double-indirect file's last fs block is reachable",
             big["data_blocks"][-1] in referenced)
    v.ok("fixture contains a directory with a child file",
         any(r["type"] == "file" and r["parent"] != 0 for r in rec.values()))
    v.ok("fixture contains a zero-length file",
         any(r["type"] == "file" and r["size"] == 0 for r in rec.values()))
    v.ok("fixture contains a 31-byte name",
         any(len(f) == NAME_MAX for f in rec))
    return v.report()


def main(argv=None):
    ap = argparse.ArgumentParser(description="Verify a VimtuFS2 image on the host")
    ap.add_argument("image", nargs="?", default=str(ROOT / "build" / "vimtufs2.img"))
    ap.add_argument("--manifest", default=None)
    args = ap.parse_args(argv)
    manifest = args.manifest
    if manifest is None:
        manifest = str(Path(args.image).with_name("vimtufs2_manifest.json"))
        if not Path(manifest).exists():
            # v4 / negative fixtures carry their own manifest name
            stem = Path(args.image).stem.replace("vimtufs2", "vimtufs2", 1)
            manifest = str(Path(args.image).with_name(stem + "_manifest.json"))
    if not Path(args.image).exists():
        print("[vimtufs2-verify] no image at %s" % args.image)
        return 1
    if not Path(manifest).exists():
        print("[vimtufs2-verify] no manifest at %s (generate it first)" % manifest)
        return 1
    print("[vimtufs2-verify] image=%s" % args.image)
    print("[vimtufs2-verify] manifest=%s" % manifest)
    return verify(args.image, manifest)


if __name__ == "__main__":
    sys.exit(main())
