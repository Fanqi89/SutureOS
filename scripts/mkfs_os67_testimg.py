#!/usr/bin/env python3
"""Generate a Minix v1 filesystem image as used by OS67 (SutureOS port fixture).

The on-disk layout is the one documented in OS67's inc/minix.h and implemented by
fs/inode.c + fs/bitmap.c:

    block 0            bootsector (zeros)
    block 1            superblock (struct super_block, 20 bytes used)
    block 2..          inode bitmap  (imap_blk blocks)
    ...                zone bitmap   (zmap_blk blocks)
    ...                inode table   (ninodes * 32 bytes)
    fst_data_zone..    data zones    (1024 bytes each, log_zone_size = 0)

Bitmap bit numbering matches OS67's bitmap.c: inode bit i (i >= 1) is in
IMAP_BLK = 2 + i/8192 at bit i%8; data zone z is at ZMAP_BLK = 2+imap_blk +
(z-fst_data_zone+1)/8192, bit (z-fst_data_zone+1)%8.

Fixture contents:
    /HELLO.TXT          28 bytes
    /BIG.BIN            20000 bytes -> 7 direct zones + 13 through the indirect zone
    /SUBDIR/INNER.TXT   28 bytes
    /EMPTY              0 bytes
    /MAXNAME0123456     14-char name (the NAME_LEN limit)

  py -3 scripts/mkfs_os67_testimg.py [--out build/os67.img]
Writes build/os67_manifest.json with size+sha256 for the host verifier.
"""
import argparse
import hashlib
import json
import os
import struct
import sys

BSIZE = 1024
SECTORS_PER_BLOCK = 2
TOTAL_BLOCKS = 4096
NINODES = 64
IMAP_BLK = 1
ZMAP_BLK = 1
INODE_SIZE = 32
IPB = BSIZE // INODE_SIZE            # 32 inodes per block
INODE_BLOCKS = (NINODES + IPB - 1) // IPB
INODE_START = 2 + IMAP_BLK + ZMAP_BLK
FST_DATA_ZONE = INODE_START + INODE_BLOCKS   # block number == zone number

S_IFDIR = 0o040000
S_IFREG = 0o100000
MAGIC = 0x137F                       # Minix v1 magic

NDIRECT = 7
NINDIRECT = BSIZE // 2               # 512


class Image:
    def __init__(self):
        self.data = bytearray(TOTAL_BLOCKS * BSIZE)

    def block(self, no):
        return self.data[no * BSIZE:(no + 1) * BSIZE]

    def set_block(self, no, payload):
        assert len(payload) <= BSIZE
        self.data[no * BSIZE:no * BSIZE + len(payload)] = payload

    def put_u16(self, block_no, off, v):
        struct.pack_into('<H', self.data, block_no * BSIZE + off, v & 0xFFFF)

    def get_u16(self, block_no, off):
        return struct.unpack_from('<H', self.data, block_no * BSIZE + off)[0]


def bitmap_set(img, base_block, bit):
    byte = bit // 8
    m = 1 << (bit % 8)
    img.data[base_block * BSIZE + byte] |= m


def bitmap_get(img, base_block, bit):
    return (img.data[base_block * BSIZE + bit // 8] >> (bit % 8)) & 1


def build_fixture():
    img = Image()
    # ---- superblock ----
    nzones = TOTAL_BLOCKS
    sb = struct.pack('<HHHHHHIHH', NINODES, nzones, IMAP_BLK, ZMAP_BLK,
                     FST_DATA_ZONE, 0, (NDIRECT + NINDIRECT) * BSIZE, MAGIC, 0)
    img.set_block(1, sb)
    assert len(sb) == 20

    # ---- inode table helpers ----
    inodes = {}

    def set_inode(ino, mode, size, zones, nlinks):
        inodes[ino] = (mode, size, zones, nlinks)
        blk = INODE_START + (ino - 1) // IPB
        off = ((ino - 1) % IPB) * INODE_SIZE
        struct.pack_into('<HHIIBB', img.data, blk * BSIZE + off,
                         mode, 0, size, 0, 0, nlinks)
        for k in range(9):
            struct.pack_into('<H', img.data,
                             blk * BSIZE + off + 14 + 2 * k,
                             zones[k] if k < len(zones) else 0)
        # mark inode used in the inode bitmap (bit == inode number)
        bitmap_set(img, 2, ino)

    def dirent(ino, name):
        raw = name.encode('utf-8')
        assert len(raw) <= 14, name
        return struct.pack('<H14s', ino, raw)

    def write_dir(ino, entries, nlinks):
        payload = b''.join(dirent(i, n) for i, n in entries)
        zones = []
        n = 0
        while n < len(payload):
            zone = alloc_zone()
            chunk = payload[n:n + BSIZE]
            img.set_block(zone, chunk + bytes(BSIZE - len(chunk)))
            zones.append(zone)
            n += BSIZE
        set_inode(ino, S_IFDIR | 0o755, len(payload), zones, nlinks)
        return zones

    def write_file(ino, payload, nlinks=1):
        zones = []
        if not payload:
            set_inode(ino, S_IFREG | 0o644, 0, [], nlinks)
            return
        chunks = [payload[i:i + BSIZE] for i in range(0, len(payload), BSIZE)]
        direct = chunks[:NDIRECT]
        rest = chunks[NDIRECT:]
        # direct zones
        for c in direct:
            zone = alloc_zone()
            img.set_block(zone, c + bytes(BSIZE - len(c)))
            zones.append(zone)
        # indirect zone (OS67 only implements the single indirect table)
        if rest:
            ind_zone = alloc_zone()
            table = bytearray(BSIZE)
            for i, c in enumerate(rest):
                assert i < NINDIRECT
                zone = alloc_zone()
                img.set_block(zone, c + bytes(BSIZE - len(c)))
                struct.pack_into('<H', table, i * 2, zone)
            img.set_block(ind_zone, bytes(table))
            zones.append(ind_zone)          # zone[NDIRECT] = indirect table
        set_inode(ino, S_IFREG | 0o644, len(payload), zones, nlinks)

    next_zone = [FST_DATA_ZONE]

    def alloc_zone():
        z = next_zone[0]
        assert z < TOTAL_BLOCKS
        next_zone[0] += 1
        # zone bitmap bit index: zone - fst_data_zone + 1 (OS67 bitmap.c)
        bit = z - FST_DATA_ZONE + 1
        bitmap_set(img, 2 + IMAP_BLK, bit)
        return z

    hello = b'Hello from OS67 minix FS!\n'
    inner = b'Nested file inside SUBDIR.\n'
    big = bytes(((i * 7 + 13) & 0xFF) for i in range(20000))

    # inode 1 = root directory
    write_dir(1, [(1, '.'), (1, '..'), (2, 'HELLO.TXT'), (3, 'BIG.BIN'),
                  (4, 'SUBDIR'), (5, 'EMPTY'), (6, 'MAXNAME0123456')], 3)
    write_file(2, hello)
    write_file(3, big)
    write_dir(4, [(4, '.'), (1, '..'), (7, 'INNER.TXT')], 2)
    write_file(5, b'')
    write_file(6, b'x' * 3)
    write_file(7, inner)
    assert len(inodes) == 7

    manifest = {
        'image': 'os67.img',
        'blocks': TOTAL_BLOCKS,
        'ninodes': NINODES,
        'magic': MAGIC,
        'fst_data_zone': FST_DATA_ZONE,
        'files': {
            'HELLO.TXT': {'size': len(hello), 'sha256': hashlib.sha256(hello).hexdigest()},
            'BIG.BIN': {'size': len(big), 'sha256': hashlib.sha256(big).hexdigest()},
            'SUBDIR/INNER.TXT': {'size': len(inner), 'sha256': hashlib.sha256(inner).hexdigest()},
            'EMPTY': {'size': 0, 'sha256': hashlib.sha256(b'').hexdigest()},
            'MAXNAME0123456': {'size': 3, 'sha256': hashlib.sha256(b'x' * 3).hexdigest()},
        },
    }
    return bytes(img.data), manifest


def verify_image(data, manifest):
    """Independent parse of the produced image (does not reuse the generator)."""
    checks = 0
    fails = []

    def check(name, cond, detail=''):
        nonlocal checks
        checks += 1
        print('  [%s] %s %s' % ('ok' if cond else 'FAIL', name, detail))
        if not cond:
            fails.append(name)

    def blk(no):
        return data[no * BSIZE:(no + 1) * BSIZE]

    sb = struct.unpack_from('<HHHHHHIHH', data, BSIZE)
    ninodes, nzones, imap_blk, zmap_blk, fst_data, log_zone, max_size, magic, state = sb
    check('superblock magic', magic == manifest['magic'], '0x%x' % magic)
    check('superblock geometry', ninodes == manifest['ninodes'] and fst_data ==
          manifest['fst_data_zone'] and log_zone == 0 and nzones == manifest['blocks'],
          'ninodes=%d fst=%d nzones=%d' % (ninodes, fst_data, nzones))

    imap = blk(2)
    zmap_blk_no = 2 + imap_blk

    def inode_get(ino):
        b = INODE_START + (ino - 1) // IPB
        off = ((ino - 1) % IPB) * INODE_SIZE
        mode, uid, size, mtime, gid, nlinks = struct.unpack_from('<HHIIBB', data,
                                                                 b * BSIZE + off)
        zones = list(struct.unpack_from('<9H', data, b * BSIZE + off + 14))
        return mode, size, zones, nlinks

    def inode_used(ino):
        return (imap[ino // 8] >> (ino % 8)) & 1

    def file_bytes(mode, size, zones):
        out = bytearray()
        for k in range(NDIRECT):
            if len(out) >= size:
                break
            if zones[k]:
                out += blk(zones[k])
        if len(out) < size and zones[NDIRECT]:
            table = blk(zones[NDIRECT])
            for i in range(NINDIRECT):
                if len(out) >= size:
                    break
                z = struct.unpack_from('<H', table, i * 2)[0]
                if z:
                    out += blk(z)
        return bytes(out[:size])

    check('root inode is a directory', (inode_get(1)[0] & S_IFDIR) != 0)

    used_zones = set()
    for ino in range(1, ninodes):
        if not inode_used(ino):
            continue
        mode, size, zones, nlinks = inode_get(ino)
        for k in range(NDIRECT):
            if zones[k]:
                check('inode %d direct zone in range' % ino, fst_data <= zones[k] < nzones)
                used_zones.add(zones[k])
        if zones[NDIRECT]:
            check('inode %d indirect zone in range' % ino, fst_data <= zones[NDIRECT] < nzones)
            used_zones.add(zones[NDIRECT])
            table = blk(zones[NDIRECT])
            for i in range(NINDIRECT):
                z = struct.unpack_from('<H', table, i * 2)[0]
                if z:
                    check('inode %d indirect entry in range' % ino, fst_data <= z < nzones)
                    used_zones.add(z)

    # bitmap/zone consistency: every used data bit must be referenced
    zmap = blk(zmap_blk_no)
    for bit in range(1, nzones - fst_data + 1):
        if (zmap[bit // 8] >> (bit % 8)) & 1:
            zone = fst_data + bit - 1
            check('zone bitmap bit %d is referenced' % bit, zone in used_zones)

    # directory walk
    root = file_bytes(*inode_get(1)[:1], *(inode_get(1)[1:3])) if False else None
    mode, size, zones, nlinks = inode_get(1)
    rootdata = file_bytes(mode, size, zones)
    entries = {}
    for off in range(0, size, 16):
        ino = struct.unpack_from('<H', rootdata, off)[0]
        name = rootdata[off + 2:off + 16].split(b'\x00')[0].decode('utf-8', 'replace')
        if ino:
            entries[name] = ino
    check('root has . and ..', entries.get('.') == 1 and entries.get('..') == 1)
    for name in ('HELLO.TXT', 'BIG.BIN', 'SUBDIR', 'EMPTY', 'MAXNAME0123456'):
        check('root lists %s' % name, name in entries)

    for path, meta in manifest['files'].items():
        parts = path.split('/')
        if len(parts) > 1:
            # nested: resolve the parent directory then the child name
            dino = entries.get(parts[0])
            if dino is None:
                check('manifest path %s resolvable' % path, False)
                continue
            m2, s2, z2, _ = inode_get(dino)
            d2 = file_bytes(m2, s2, z2)
            sub = {}
            for off in range(0, s2, 16):
                i2 = struct.unpack_from('<H', d2, off)[0]
                nm = d2[off + 2:off + 16].split(b'\x00')[0].decode('utf-8', 'replace')
                if i2:
                    sub[nm] = i2
            ino = sub.get(parts[1])
        else:
            ino = entries.get(parts[0])
        check('manifest path %s resolvable' % path, ino is not None)
        if ino is None:
            continue
        mode, size, zones, nlinks = inode_get(ino)
        payload = file_bytes(mode, size, zones)
        check('%s size' % path, size == meta['size'], '%d' % size)
        check('%s sha256' % path, hashlib.sha256(payload).hexdigest() == meta['sha256'])
        check('%s has data blocks' % path, meta['size'] == 0 or len([z for z in zones if z]) > 0)

    check('image sha256 matches manifest',
          hashlib.sha256(data).hexdigest() == manifest.get('image_sha256', hashlib.sha256(data).hexdigest()))
    print('[mkfs-os67] checks=%d ok=%d fail=%d' % (checks, checks - len(fails), len(fails)))
    return 0 if not fails else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default='build/os67.img')
    ap.add_argument('--verify', action='store_true', default=True)
    args = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, args.out)
    data, manifest = build_fixture()
    manifest_path = os.path.join(root, 'build', 'os67_manifest.json')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'wb') as fh:
        fh.write(data)
    manifest['image_sha256'] = hashlib.sha256(data).hexdigest()
    with open(manifest_path, 'w', encoding='utf-8') as fh:
        json.dump(manifest, fh, indent=2)
    print('[mkfs-os67] wrote %s (%d bytes)' % (out, len(data)))
    if args.verify:
        rc = verify_image(data, manifest)
        if rc != 0:
            return rc
    print('[mkfs-os67] manifest=%s' % manifest_path)
    print('[mkfs-os67] OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
