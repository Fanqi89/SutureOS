#!/usr/bin/env python3
"""Host-side independent verifier for the OS67 (Minix v1) fixture image.

Parses the image from scratch (no generator or kernel code is imported) and
checks: superblock geometry/magic, inode bitmap vs. inode table, zone bitmap
consistency (every allocated zone is referenced by exactly the inode chain that
claims it), direct + single-indirect block mapping, directory structure and the
manifest sha256 of each file.

  py -3 scripts/verify_os67_read.py build/os67.img [--manifest build/os67_manifest.json]
"""
import argparse
import hashlib
import json
import os
import struct
import sys

BSIZE = 1024
INODE_SIZE = 32
IPB = BSIZE // INODE_SIZE
NDIRECT = 7
NINDIRECT = BSIZE // 2
S_IFMT = 0xF000
S_IFDIR = 0x4000
S_IFREG = 0x8000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image', nargs='?', default='build/os67.img')
    ap.add_argument('--manifest', default='build/os67_manifest.json')
    args = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    image = args.image if os.path.isabs(args.image) else os.path.join(root, args.image)
    manifest_path = args.manifest if os.path.isabs(args.manifest) else os.path.join(root, args.manifest)
    data = open(image, 'rb').read()
    manifest = json.load(open(manifest_path, encoding='utf-8'))

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

    ninodes, nzones, imap_blk, zmap_blk, fst_data, log_zone, max_size, magic, state = \
        struct.unpack_from('<HHHHHHIHH', data, BSIZE)
    check('superblock magic', magic in (0x137F, 0x138F), '0x%x' % magic)
    check('zone size is 1024 (log_zone_size=0)', log_zone == 0)
    check('geometry sane', ninodes == manifest['ninodes'] and nzones == manifest['blocks']
          and fst_data > 2 and fst_data < nzones,
          'ninodes=%d nzones=%d fst=%d' % (ninodes, nzones, fst_data))
    check('inode table starts before the data area',
          2 + imap_blk + zmap_blk <= fst_data)

    imap = blk(2)
    zmap = blk(2 + imap_blk)
    inode_start = 2 + imap_blk + zmap_blk
    ref_zones = {}          # zone -> inode that references it

    def inode_used(ino):
        return (imap[ino // 8] >> (ino % 8)) & 1

    def inode_get(ino):
        if not inode_used(ino):
            return None
        b = inode_start + (ino - 1) // IPB
        off = ((ino - 1) % IPB) * INODE_SIZE
        mode, uid, size, mtime, gid, nlinks = struct.unpack_from('<HHIIBB', data, b * BSIZE + off)
        zones = list(struct.unpack_from('<9H', data, b * BSIZE + off + 14))
        return {'mode': mode, 'size': size, 'zones': zones, 'nlinks': nlinks}

    def claim(zone, ino, what):
        check('zone %d (%s of inode %d) inside the data area' % (zone, what, ino),
              fst_data <= zone < nzones)
        check('zone %d referenced only once' % zone, zone not in ref_zones,
              'also by inode %s' % ref_zones.get(zone))
        ref_zones[zone] = ino

    def file_bytes(ip):
        out = bytearray()
        for k in range(NDIRECT):
            z = ip['zones'][k]
            if z:
                out += blk(z)
        if ip['zones'][NDIRECT]:
            zd = ip['zones'][NDIRECT]
            table = blk(zd)
            for i in range(NINDIRECT):
                z = struct.unpack_from('<H', table, i * 2)[0]
                if z:
                    out += blk(z)
        return bytes(out[:ip['size']])

    payloads = {}
    for ino in range(1, ninodes):
        ip = inode_get(ino)
        if not ip:
            continue
        check('inode %d has a valid mode' % ino,
              (ip['mode'] & S_IFMT) in (S_IFDIR, S_IFREG), '0x%x' % ip['mode'])
        check('inode %d size within the max file size' % ino, ip['size'] <= max_size)
        if ip['size']:
            payload = file_bytes(ip)
            check('inode %d payload length matches i_size' % ino, len(payload) == ip['size'])
            payloads[ino] = payload

    # claim every referenced zone exactly once (second pass, per inode)
    for ino in range(1, ninodes):
        ip = inode_get(ino)
        if not ip:
            continue
        for k in range(NDIRECT):
            z = ip['zones'][k]
            if z:
                claim(z, ino, 'direct')
        if ip['zones'][NDIRECT]:
            zd = ip['zones'][NDIRECT]
            claim(zd, ino, 'indirect')
            table = blk(zd)
            for i in range(NINDIRECT):
                z = struct.unpack_from('<H', table, i * 2)[0]
                if z:
                    claim(z, ino, 'indirect data')

    bitmap_used = 0
    for bit in range(1, nzones - fst_data + 1):
        if (zmap[bit // 8] >> (bit % 8)) & 1:
            bitmap_used += 1
            zone = fst_data + bit - 1
            check('zone bitmap bit %d refers to a referenced zone' % bit, zone in ref_zones)
    check('zone bitmap allocation count equals referenced zones', bitmap_used == len(ref_zones),
          'bitmap=%d referenced=%d' % (bitmap_used, len(ref_zones)))

    # directory tree walk (root inode 1)
    root_ip = inode_get(1)
    check('root inode is a directory', root_ip is not None and (root_ip['mode'] & S_IFMT) == S_IFDIR)
    rootdata = payloads.get(1, b'') if root_ip else b''
    entries = {}
    for off in range(0, len(rootdata), 16):
        ino = struct.unpack_from('<H', rootdata, off)[0]
        name = rootdata[off + 2:off + 16].split(b'\x00')[0].decode('utf-8', 'replace')
        if ino:
            entries[name] = ino
    check('root has . and ..', entries.get('.') == 1 and entries.get('..') == 1)
    for name in ('HELLO.TXT', 'BIG.BIN', 'SUBDIR', 'EMPTY', 'MAXNAME0123456'):
        check('root lists %s' % name, name in entries)

    # nested directory + manifest comparison
    sub = {}
    if 'SUBDIR' in entries:
        sip = inode_get(entries['SUBDIR'])
        check('SUBDIR is a directory', sip is not None and (sip['mode'] & S_IFMT) == S_IFDIR)
        sdata = payloads.get(entries['SUBDIR'], b'') if sip else b''
        for off in range(0, len(sdata), 16):
            ino = struct.unpack_from('<H', sdata, off)[0]
            name = sdata[off + 2:off + 16].split(b'\x00')[0].decode('utf-8', 'replace')
            if ino:
                sub[name] = ino
        check('SUBDIR lists INNER.TXT', 'INNER.TXT' in sub)

    for path, meta in manifest['files'].items():
        parts = path.split('/')
        if len(parts) == 1:
            ino = entries.get(parts[0])
        else:
            ino = sub.get(parts[1]) if parts[0] == 'SUBDIR' else None
        check('%s resolvable' % path, ino is not None)
        if ino is None:
            continue
        ip = inode_get(ino)
        payload = payloads.get(ino, b'')
        check('%s size' % path, len(payload) == meta['size'], '%d' % len(payload))
        check('%s sha256' % path, hashlib.sha256(payload).hexdigest() == meta['sha256'])

    check('image sha256 matches the manifest',
          hashlib.sha256(data).hexdigest() == manifest.get('image_sha256'))

    print('[os67fs-verify] checks=%d ok=%d fail=%d' % (checks, checks - len(fails), len(fails)))
    print('[os67fs-verify] RESULT:', 'PASS' if not fails else 'FAIL')
    return 0 if not fails else 1


if __name__ == '__main__':
    sys.exit(main())
