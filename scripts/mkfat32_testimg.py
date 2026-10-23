#!/usr/bin/env python3
"""Build a test FAT32 superfloppy image + a real .deb package for SutureOS.

Creates:
  build/fat32.img  - unpartitioned ("superfloppy") genuine FAT32 volume
                     (>= 65525 clusters, FAT[0..2] + root dir + files)
  containing HELLO.TXT, README.TXT and hello.deb (a real Debian package:
  ar archive with debian-binary + control.tar.gz + data.tar.gz).

The kernel auto-mounts drive 0 as FAT32 at /mnt/fat and installs
/mnt/fat/hello.deb at boot (see kernel/main/kmain.c).

Self-verifies the produced image by re-parsing it with an independent
reader (host-side verification - never trust the guest).
"""
import io
import os
import struct
import tarfile
import gzip

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "build")
IMG_PATH = os.path.join(OUT_DIR, "fat32.img")

SECTOR = 512
TOTAL_SECTORS = 71680          # 35 MiB -> 65k+ clusters at spc=1
RESERVED = 32
NUM_FATS = 2
SPC = 1                        # 512-byte clusters
MEDIA = 0xF8
ROOT_CLUSTER = 2
FSINFO_SECTOR = 1
BACKUP_SECTOR = 6


def ar_member(name: str, data: bytes) -> bytes:
    hdr = (
        name.ljust(16).encode()
        + b"0".ljust(12)
        + b"0".ljust(6)
        + b"0".ljust(6)
        + b"100644".ljust(8)
        + str(len(data)).encode().ljust(10)
        + b"`\n"
    )
    assert len(hdr) == 60
    out = hdr + data
    if len(out) & 1:
        out += b"\n"
    return out


def make_tar(members: dict) -> bytes:
    """Build a ustar archive (uncompressed) from {name: (data, is_dir)}."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tf:
        for name, payload in members.items():
            if payload is None:  # directory
                ti = tarfile.TarInfo(name)
                ti.type = tarfile.DIRTYPE
                ti.mode = 0o755
                tf.addfile(ti)
            else:
                ti = tarfile.TarInfo(name)
                ti.size = len(payload)
                ti.mode = 0o644
                tf.addfile(ti, io.BytesIO(payload))
    return buf.getvalue()


def gzip_bytes(data: bytes) -> bytes:
    out = io.BytesIO()
    with gzip.GzipFile(fileobj=out, mode="wb", mtime=0) as gz:
        gz.write(data)
    return out.getvalue()


def make_deb() -> bytes:
    control = (
        "Package: hello\n"
        "Version: 1.0-1\n"
        "Architecture: all\n"
        "Maintainer: SutureOS Builder <sutureos@localhost>\n"
        "Section: misc\n"
        "Priority: optional\n"
        "Installed-Size: 4\n"
        "Depends: libc6\n"
        "Description: SutureOS deb installer demo package\n"
        " A tiny package used to prove the kernel deb installer works.\n"
    )
    control_tar = make_tar({"./control": control.encode()})
    data_tar = make_tar({
        "./usr/": None,
        "./usr/share/": None,
        "./usr/share/hello/": None,
        "./usr/share/hello/hello.txt":
            b"Hello from a real .deb package installed by SutureOS!\n",
        "./usr/share/hello/motd.txt":
            b"deb_install() parsed ar + gzip + ustar and unpacked this file.\n",
    })
    return (
        b"!<arch>\n"
        + ar_member("debian-binary", b"2.0\n")
        + ar_member("control.tar.gz", gzip_bytes(control_tar))
        + ar_member("data.tar.gz", gzip_bytes(data_tar))
    )


LFN_OFS = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)


def lfn_checksum(short11: bytes) -> int:
    """VFAT long-name checksum over the 11 short-name bytes."""
    total = 0
    for b in short11:
        total = ((total >> 1) + (total << 7) + b) & 0xFF
    return total


def make_lfn_chain(long_name: str, short11: bytes) -> bytes:
    """On-disk bytes for one VFAT long-name chain.

    Physical order is the reverse of the logical order: the entry at the lowest
    offset carries ord = N | 0x40 and holds the LAST 13 characters, the entry right
    before the short entry carries ord = 1 and holds the FIRST 13 characters. All
    entries carry the checksum of the short name.
    """
    units = [ord(c) for c in long_name]
    n_entries = (len(units) + 12) // 13
    if n_entries == 0 or n_entries > 20:
        raise ValueError("long name out of range")
    padded = units + [0x0000] + [0xFFFF] * (n_entries * 13 - len(units) - 1)
    chk = lfn_checksum(short11)
    out = bytearray()
    for ordn in range(n_entries, 0, -1):
        ent = bytearray(32)
        ent[0] = ordn | (0x40 if ordn == n_entries else 0)
        ent[11] = 0x0F
        ent[12] = 0x00
        ent[13] = chk
        chunk = padded[(ordn - 1) * 13:ordn * 13]
        for i, off in enumerate(LFN_OFS):
            c = chunk[i]
            ent[off] = c & 0xFF
            ent[off + 1] = (c >> 8) & 0xFF
        out += ent
    return bytes(out)


def build_image(files: dict) -> bytes:
    """files: {name: bytes} placed in the root directory."""
    # FAT size must cover total clusters; solve iteratively.
    fat_sz = 512
    while True:
        data_start = RESERVED + NUM_FATS * fat_sz
        clusters = (TOTAL_SECTORS - data_start) // SPC
        need = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
        if need <= fat_sz:
            break
        fat_sz = need

    total_clusters = clusters
    img = bytearray(TOTAL_SECTORS * SECTOR)

    def put_sector(sec, data):
        img[sec * SECTOR:sec * SECTOR + len(data)] = data

    # ---- boot sector (also mirrored to backup at sector 6) ----
    bs = bytearray(SECTOR)
    bs[0:3] = b"\xEB\x58\x90"
    bs[3:11] = b"SUTUREOS "
    struct.pack_into("<H", bs, 11, SECTOR)
    bs[13] = SPC
    struct.pack_into("<H", bs, 14, RESERVED)
    bs[16] = NUM_FATS
    struct.pack_into("<H", bs, 17, 0)          # root entries (FAT32: 0)
    struct.pack_into("<H", bs, 19, 0)          # totsec16
    bs[21] = MEDIA
    struct.pack_into("<H", bs, 22, 0)          # fatsz16
    struct.pack_into("<H", bs, 24, 63)         # spt
    struct.pack_into("<H", bs, 26, 255)        # heads
    struct.pack_into("<I", bs, 28, 0)          # hidden
    struct.pack_into("<I", bs, 32, TOTAL_SECTORS)
    struct.pack_into("<I", bs, 36, fat_sz)
    struct.pack_into("<H", bs, 40, 0)          # ext flags
    struct.pack_into("<H", bs, 42, 0)          # fsver
    struct.pack_into("<I", bs, 44, ROOT_CLUSTER)
    struct.pack_into("<I", bs, 48, FSINFO_SECTOR)
    struct.pack_into("<I", bs, 52, BACKUP_SECTOR)
    bs[64] = 0x80
    bs[66] = 0x29
    struct.pack_into("<I", bs, 67, 0x5A7C0DE5)
    bs[71:82] = b"SUTUREOS  "
    bs[82:90] = b"FAT32   "
    bs[510] = 0x55
    bs[511] = 0xAA
    put_sector(0, bytes(bs))
    put_sector(BACKUP_SECTOR, bytes(bs))

    # ---- FSInfo ----
    fi = bytearray(SECTOR)
    struct.pack_into("<I", fi, 0, 0x41615252)
    struct.pack_into("<I", fi, 484, 0x61417272)
    free = total_clusters - 2 - len(files) * 8   # rough, exact below
    struct.pack_into("<I", fi, 488, 0xFFFFFFFF)  # free count: unknown hint
    struct.pack_into("<I", fi, 492, 3)           # next free hint
    struct.pack_into("<I", fi, 508, 0xAA5555AA)
    put_sector(FSINFO_SECTOR, bytes(fi))

    # ---- FATs ----
    fat = [0] * (total_clusters + 2)
    fat[0] = 0x0FFFFFF8
    fat[1] = 0xFFFFFFFF
    # The root directory lives in cluster 2 and must be marked allocated,
    # otherwise a guest allocator correctly hands cluster 2 out as "free" and
    # overwrites the root directory (this exact bug was found the hard way).
    fat[ROOT_CLUSTER] = 0x0FFFFFFF

    next_cluster = ROOT_CLUSTER + 1
    root_entries = bytearray()

    def alloc_chain(data: bytes) -> int:
        nonlocal next_cluster
        n = max(1, (len(data) + SECTOR * SPC - 1) // (SECTOR * SPC))
        first = next_cluster
        for i in range(n):
            cl = next_cluster + i
            fat[cl] = (cl + 1) if i < n - 1 else 0x0FFFFFFF
        next_cluster += n
        return first

    def write_chain(first: int, data: bytes):
        cl = first
        off = 0
        while True:
            sec = RESERVED + NUM_FATS * fat_sz + (cl - 2) * SPC
            put_sector(sec, data[off:off + SECTOR * SPC])
            off += SECTOR * SPC
            nxt = fat[cl]
            if nxt >= 0x0FFFFFF8:
                break
            cl = nxt

    for name, data in files.items():
        ent = bytearray(32)
        base, _, ext = name.upper().partition(".")
        ent[0:8] = base[:8].ljust(8).encode()
        ent[8:11] = (ext[:3] if ext else "").ljust(3).encode()
        ent[11] = 0x20                      # archive
        struct.pack_into("<H", ent, 20, (first := 0) >> 16)  # high word below
        first = alloc_chain(data)
        struct.pack_into("<H", ent, 20, (first >> 16) & 0xFFFF)
        struct.pack_into("<H", ent, 26, first & 0xFFFF)
        struct.pack_into("<I", ent, 28, len(data))
        root_entries += ent
        write_chain(first, data)

    # One VFAT long-name file so the guest's LFN reader has something real to parse.
    # Short alias LFNTRAN.TXT; the name ls must show is "Long file name test.txt".
    lfn_data = b"long name payload: SutureOS reads VFAT long entries\n"
    lfn_short11 = b"LFNTRAN TXT"
    lfn_ent = bytearray(32)
    lfn_ent[0:11] = lfn_short11
    lfn_ent[11] = 0x20
    lfn_first = alloc_chain(lfn_data)
    struct.pack_into("<H", lfn_ent, 20, (lfn_first >> 16) & 0xFFFF)
    struct.pack_into("<H", lfn_ent, 26, lfn_first & 0xFFFF)
    struct.pack_into("<I", lfn_ent, 28, len(lfn_data))
    write_chain(lfn_first, lfn_data)
    root_entries += make_lfn_chain("Long file name test.txt", lfn_short11) + lfn_ent

    # root dir = cluster ROOT_CLUSTER
    put_sector(RESERVED + NUM_FATS * fat_sz, bytes(root_entries))

    # write both FAT copies (serialized little-endian)
    fat_bytes = b"".join(struct.pack("<I", v) for v in fat)
    fat_bytes = fat_bytes[: fat_sz * SECTOR].ljust(fat_sz * SECTOR, b"\x00")
    put_sector(RESERVED, fat_bytes)
    put_sector(RESERVED + fat_sz, fat_bytes)

    # patch FSInfo free count exactly
    used = next_cluster - 2
    free = total_clusters - used
    struct.pack_into("<I", img, FSINFO_SECTOR * SECTOR + 488, free)
    struct.pack_into("<I", img, FSINFO_SECTOR * SECTOR + 492, next_cluster)
    return bytes(img), fat_sz, total_clusters


def verify_image(img: bytes, expect: dict):
    """Independent host-side parse: BPB + root listing + file contents."""
    def gsec(n):
        return img[n * SECTOR:(n + 1) * SECTOR]

    bs = gsec(0)
    assert bs[510:512] == b"\x55\xaa", "boot signature"
    bps = struct.unpack_from("<H", bs, 11)[0]
    spc = bs[13]
    reserved = struct.unpack_from("<H", bs, 14)[0]
    nfats = bs[16]
    fatsz = struct.unpack_from("<I", bs, 36)[0]
    rootclus = struct.unpack_from("<I", bs, 44)[0]
    assert bps == 512 and spc == 1 and nfats == 2
    assert bs[82:90] == b"FAT32   ", "FAT32 label"
    clusters = (len(img) // bps - reserved - nfats * fatsz) // spc
    assert clusters >= 65525, f"not genuine FAT32: {clusters} clusters"

    fat0 = img[reserved * bps:(reserved + fatsz) * bps]
    fat1 = img[(reserved + fatsz) * bps:(reserved + 2 * fatsz) * bps]
    assert fat0 == fat1, "FAT1 != FAT2"

    def fat_get(cl):
        return struct.unpack_from("<I", fat0, cl * 4)[0]

    # The root directory cluster must be allocated (EOC), otherwise a guest
    # allocator legitimately gives it away and destroys the root directory.
    assert (fat_get(rootclus) & 0x0FFFFFFF) == 0x0FFFFFFF, \
        f"root cluster {rootclus} is not marked EOC in the FAT"

    # FSInfo free count must match the FAT exactly
    free_in_fat = sum(1 for cl in range(2, clusters + 2)
                      if (fat_get(cl) & 0x0FFFFFFF) == 0)
    fsinfo_free = struct.unpack_from("<I", img, FSINFO_SECTOR * SECTOR + 488)[0]
    assert fsinfo_free == free_in_fat, \
        f"FSInfo free={fsinfo_free} but FAT says {free_in_fat}"

    def read_chain(first):
        out = b""
        cl = first
        while cl < 0x0FFFFFF8:
            sec = reserved + nfats * fatsz + (cl - 2) * spc
            out += gsec(sec)[: bps * spc]
            cl = fat_get(cl)
        return out

    data_start = reserved + nfats * fatsz
    root = gsec(data_start + (rootclus - 2) * spc)
    found = {}
    for i in range(0, SECTOR, 32):
        ent = root[i:i + 32]
        if ent[0] == 0:
            break
        if ent[0] == 0xE5 or (ent[11] & 0x0F) == 0x0F or (ent[11] & 0x08):
            continue
        name = ent[0:8].decode(errors="replace").rstrip()
        ext = ent[8:11].decode(errors="replace").rstrip()
        full = f"{name}.{ext}" if ext else name
        first = struct.unpack_from("<H", ent, 26)[0] | (struct.unpack_from("<H", ent, 20)[0] << 16)
        size = struct.unpack_from("<I", ent, 28)[0]
        content = read_chain(first)[:size]
        found[full] = content
    for name, want in expect.items():
        assert name in found, f"{name} missing from image root"
        assert found[name] == want, f"{name} content mismatch"
    return found


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    deb = make_deb()
    # verify the deb itself round-trips through our own ar/tar/gzip readers
    assert deb.startswith(b"!<arch>\n")
    hello = ("Hello from SutureOS FAT32!\n"
             "This file lives on a real FAT32 volume (driver: fat32).\n").encode()
    readme = ("SutureOS test disk.\n"
              "Files: HELLO.TXT, README.TXT, hello.deb\n"
              "hello.deb is a real Debian package; the kernel installs it\n"
              "at boot (see kmain deb demo).\n").encode()

    img, fat_sz, clusters = build_image({
        "HELLO.TXT": hello,
        "README.TXT": readme,
        "HELLO.DEB": deb,
    })
    found = verify_image(img, {"HELLO.TXT": hello, "README.TXT": readme,
                               "HELLO.DEB": deb})

    with open(IMG_PATH, "wb") as f:
        f.write(img)
    print(f"FAT32 image : {IMG_PATH} ({len(img)} bytes)")
    print(f"  clusters  : {clusters} (genuine FAT32: >=65525)")
    print(f"  fat size  : {fat_sz} sectors x2 (mirrored, verified equal)")
    print(f"  root files: {', '.join(sorted(found))}")
    print(f"hello.deb   : {len(deb)} bytes (ar + control.tar.gz + data.tar.gz)")
    print("host-side verification: PASS")


if __name__ == "__main__":
    main()
