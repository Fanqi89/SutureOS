#!/usr/bin/env python3
"""Build a genuine MayFS test volume for the SutureOS MayFS port.

The format is MayOS' MayFS, read from MayOS_0.3.0-pre1/kernel/fs.c + core.h
(repository LICENSE = MIT, Copyright (c) 2024 MayOS Project).  This generator is
written from the byte-level spec in docs/mayfs-format-notes.md and re-implements
the layout independently of the kernel driver:

  sector 512 bytes
  primary slot : LBA 0..511    (bytes        0 .. 262143)
  backup  slot : LBA 512..1023 (bytes   262144 .. 524287)
  slot header  : 0x00 magic "MAYFS001"
                 0x08 u32 FNV-1a(table)      (table = 48 * 4160 bytes @ 0x200)
                 0x0C "MAY2"                 0x10 u32 generation
                 0x14 u32 ~generation
  file table   : offset 512, 48 records of {u32 used,dir,parent,size;
                 char name[48]; u8 data[4096]} = 4160 bytes each (199680 total)
  prefs record : offset 500*512 = 256000: "MAYPREF1", u32 FNV-1a(data[32]), data

Fixture contents (all volumes):
  record 0  "disk1"      dir  parent 0   (MayFS root record)
  record 1  "Documents"  dir  parent 0   (records 0..3 MUST be root dirs)
  record 2  "Desktop"    dir  parent 0
  record 3  "Trash"      dir  parent 0
  record 4  "HELLO.TXT"  file parent 0
  record 5  "BIG.BIN"    file parent 0   -> 4095 bytes, the largest legal file
  record 6  "SUBDIR"     dir  parent 0
  record 7  "INNER.TXT"  file parent 6
  record 8  "EMPTY"      file parent 0   -> 0 bytes
  record 9  <47-char name> file parent 0 -> longest legal name
  record 10 "ONE.TXT"    file parent 0
  record 11 "DOC.TXT"    file parent 1   (inside a mandatory dir)

Note on "> 4 KiB": MayFS cannot store it.  FILE_CAP is 4096 and the format
requires data[size] == 0, so the largest legal payload is 4095 bytes.  BIG.BIN
is therefore the biggest file the filesystem can represent; the port report
records that limit instead of pretending a larger file exists.

  py -3 scripts/mkfs_mayfs_testimg.py            # build + self-verify
  py -3 scripts/mkfs_mayfs_testimg.py --verify   # verify the existing image
"""
import argparse
import hashlib
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, 'build')

SECTOR = 512
SLOT_SECTORS = 512
SLOT_BYTES = SLOT_SECTORS * SECTOR          # 262144
BACKUP_LBA = SLOT_SECTORS
MAX_FILES = 48                              # FS_MAX
NAME_BYTES = 48
FILE_CAP = 4096
FILE_MAX = 4095
ENTRY_BYTES = 4160                          # 4*4 + 48 + 4096
TABLE_OFFSET = 512
TABLE_BYTES = MAX_FILES * ENTRY_BYTES       # 199680
PREFS_OFFSET = 500 * SECTOR                 # 256000
PREFS_LENGTH = 32
MAGIC = b'MAYFS001'
GEN_TAG = b'MAY2'
PREFS_MAGIC = b'MAYPREF1'
DEFAULT_SECTORS = 4096                      # 2 MiB
DEFAULT_GENERATION = 7

LONG_NAME = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCDEFG.TXT'   # 47 chars
assert len(LONG_NAME) == 47, len(LONG_NAME)

HELLO_TEXT = b'Hello from MayFS.\nSecond line.\n'
INNER_TEXT = b'inner file inside SUBDIR\n'
ONE_TEXT = b'one\n'
DOC_TEXT = b'document inside Documents\n'
BIG_PAYLOAD = bytes(((i * 37 + 11) & 0xFF) for i in range(FILE_MAX))

# name, parent, is_dir, payload
ENTRIES = [
    ('disk1',     0, True,  b''),
    ('Documents', 0, True,  b''),
    ('Desktop',   0, True,  b''),
    ('Trash',     0, True,  b''),
    ('HELLO.TXT', 0, False, HELLO_TEXT),
    ('BIG.BIN',   0, False, BIG_PAYLOAD),
    ('SUBDIR',    0, True,  b''),
    ('INNER.TXT', 6, False, INNER_TEXT),
    ('EMPTY',     0, False, b''),
    (LONG_NAME,   0, False, b'long name file\n'),
    ('ONE.TXT',   0, False, ONE_TEXT),
    ('DOC.TXT',   1, False, DOC_TEXT),
]

PREFS_DATA = b'MayFS prefs fixture v1\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00'
assert len(PREFS_DATA) == PREFS_LENGTH, len(PREFS_DATA)


def fnv1a32(data):
    """FNV-1a 32 over `data` - the checksum MayOS fs.c calls checksum()."""
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def build_table(entries):
    table = bytearray(TABLE_BYTES)
    for i, (name, parent, is_dir, payload) in enumerate(entries):
        assert i < MAX_FILES
        assert len(name) <= 47, name
        assert len(payload) <= FILE_MAX, len(payload)
        off = i * ENTRY_BYTES
        struct.pack_into('<IIII', table, off, 1, 1 if is_dir else 0, parent,
                         len(payload))
        nb = name.encode('utf-8')
        table[off + 16:off + 16 + len(nb)] = nb
        table[off + 64:off + 64 + len(payload)] = payload
        # data[size] stays 0 - the format's NUL-after-payload invariant
    return bytes(table)


def build_slot(table, generation, prefs=PREFS_DATA):
    slot = bytearray(SLOT_BYTES)
    slot[0:8] = MAGIC
    struct.pack_into('<I', slot, 8, fnv1a32(table))
    slot[12:16] = GEN_TAG
    struct.pack_into('<I', slot, 16, generation & 0xFFFFFFFF)
    struct.pack_into('<I', slot, 20, (~generation) & 0xFFFFFFFF)
    slot[TABLE_OFFSET:TABLE_OFFSET + TABLE_BYTES] = table
    if prefs is not None:
        slot[PREFS_OFFSET:PREFS_OFFSET + 8] = PREFS_MAGIC
        struct.pack_into('<I', slot, PREFS_OFFSET + 8, fnv1a32(prefs))
        slot[PREFS_OFFSET + 12:PREFS_OFFSET + 12 + PREFS_LENGTH] = prefs
    return bytes(slot)


def build_image(generation=DEFAULT_GENERATION, sectors=DEFAULT_SECTORS):
    assert sectors >= 2 * SLOT_SECTORS, sectors
    table = build_table(ENTRIES)
    slot = build_slot(table, generation)
    img = bytearray(sectors * SECTOR)
    img[0:SLOT_BYTES] = slot
    img[BACKUP_LBA * SECTOR:BACKUP_LBA * SECTOR + SLOT_BYTES] = slot
    return bytes(img), table, slot


# ==================== independent verification ====================

def parse_slot(slot, label, errors):
    if slot[0:8] != MAGIC:
        errors.append('%s: magic %r != %r' % (label, slot[0:8], MAGIC))
        return None
    checksum = struct.unpack_from('<I', slot, 8)[0]
    table = slot[TABLE_OFFSET:TABLE_OFFSET + TABLE_BYTES]
    if checksum != fnv1a32(table):
        errors.append('%s: table checksum 0x%08x != 0x%08x'
                      % (label, checksum, fnv1a32(table)))
        return None
    if slot[12:16] != GEN_TAG:
        errors.append('%s: generation tag %r != %r' % (label, slot[12:16], GEN_TAG))
        return None
    gen = struct.unpack_from('<I', slot, 16)[0]
    if struct.unpack_from('<I', slot, 20)[0] != ((~gen) & 0xFFFFFFFF):
        errors.append('%s: generation complement mismatch' % label)
        return None
    records = []
    for i in range(MAX_FILES):
        off = i * ENTRY_BYTES
        used, is_dir, parent, size = struct.unpack_from('<IIII', slot,
                                                        TABLE_OFFSET + off)
        name = slot[TABLE_OFFSET + off + 16:TABLE_OFFSET + off + 64]
        data = slot[TABLE_OFFSET + off + 64:TABLE_OFFSET + off + 64 + FILE_CAP]
        if not used:
            records.append(None)
            continue
        if used != 1 or is_dir > 1 or parent >= MAX_FILES or size >= FILE_CAP:
            errors.append('%s: record %d header invalid' % (label, i))
            return None
        if name[NAME_BYTES - 1] != 0:
            errors.append('%s: record %d name[47] != 0' % (label, i))
            return None
        if data[size] != 0:
            errors.append('%s: record %d data[%d] != 0' % (label, i, size))
            return None
        try:
            text = name.split(b'\x00', 1)[0].decode('utf-8')
        except UnicodeDecodeError:
            errors.append('%s: record %d name is not UTF-8' % (label, i))
            return None
        records.append({'index': i, 'name': text, 'dir': bool(is_dir),
                        'parent': parent, 'size': size, 'data': data[:size]})
    for i in range(4):
        r = records[i]
        if not r or not r['dir'] or r['parent'] != 0:
            errors.append('%s: fixed root dir record %d missing/wrong' % (label, i))
            return None
    for i in range(1, MAX_FILES):
        r = records[i]
        if not r:
            continue
        p, depth = r['parent'], 0
        while p:
            if p >= MAX_FILES or not records[p] or not records[p]['dir']:
                errors.append('%s: record %d parent chain broken at %d'
                              % (label, i, p))
                return None
            depth += 1
            if depth >= MAX_FILES:
                errors.append('%s: record %d parent chain is a cycle' % (label, i))
                return None
            p = records[p]['parent']
    prefs = None
    if slot[PREFS_OFFSET:PREFS_OFFSET + 8] == PREFS_MAGIC:
        pc = struct.unpack_from('<I', slot, PREFS_OFFSET + 8)[0]
        pdata = slot[PREFS_OFFSET + 12:PREFS_OFFSET + 12 + PREFS_LENGTH]
        if pc != fnv1a32(pdata):
            errors.append('%s: prefs checksum mismatch' % label)
        else:
            prefs = bytes(pdata)
    return {'generation': gen, 'records': records, 'prefs': prefs,
            'checksum': checksum, 'table': bytes(table), 'slot': bytes(slot)}


def verify_image(path):
    with open(path, 'rb') as fh:
        img = fh.read()
    errors = []
    checks = []

    def check(name, cond):
        checks.append((name, bool(cond)))
        if not cond:
            errors.append(name)

    check('image holds both 256 KiB slots (>= 1 MiB)', len(img) >= 2 * SLOT_BYTES)
    if len(img) < 2 * SLOT_BYTES:
        return checks, errors, None
    primary = parse_slot(img[:SLOT_BYTES], 'primary', errors)
    backup = parse_slot(img[BACKUP_LBA * SECTOR:BACKUP_LBA * SECTOR + SLOT_BYTES],
                        'backup', errors)
    check('primary slot parses (magic/checksum/generation tag)', primary is not None)
    check('backup slot parses (magic/checksum/generation tag)', backup is not None)
    if not primary or not backup:
        return checks, errors, None
    check('both slots carry the same generation',
          primary['generation'] == backup['generation'])
    check('both slot tables are byte-identical', primary['table'] == backup['table'])
    check('both slot images are byte-identical', primary['slot'] == backup['slot'])
    check('prefs record present with a valid checksum', primary['prefs'] is not None)

    recs = {r['name']: r for r in primary['records'] if r}
    by_index = primary['records']
    for name, parent, is_dir, payload in ENTRIES:
        r = recs.get(name)
        check('fixture entry %r present' % name, r is not None)
        if not r:
            continue
        check('fixture entry %r type/parent/size' % name,
              r['dir'] == is_dir and r['parent'] == parent and r['size'] == len(payload))
        check('fixture entry %r payload byte-exact' % name, r['data'] == payload)
    check('BIG.BIN is 4095 bytes (largest legal MayFS file)',
          recs.get('BIG.BIN', {}).get('size') == FILE_MAX)
    check('longest name stored is 47 characters',
          any(len(r['name']) == 47 for r in primary['records'] if r))
    check('EMPTY is a 0-byte file',
          recs.get('EMPTY', {}).get('size') == 0 and
          not recs.get('EMPTY', {}).get('dir', True))
    check('nested file SUBDIR/INNER.TXT resolves through its parent record',
          by_index[7] is not None and by_index[7]['parent'] == 6 and
          'INNER.TXT' == by_index[7]['name'])
    info = {
        'generation': primary['generation'],
        'checksum': primary['checksum'],
        'records': [{'index': r['index'], 'name': r['name'], 'dir': r['dir'],
                     'parent': r['parent'], 'size': r['size'],
                     'sha256': hashlib.sha256(r['data']).hexdigest()}
                    for r in primary['records'] if r],
        'prefs_sha256': hashlib.sha256(primary['prefs'] or b'').hexdigest(),
        'image_sha256': hashlib.sha256(img).hexdigest(),
        'image_bytes': len(img),
    }
    return checks, errors, info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(BUILD, 'mayfs.img'))
    ap.add_argument('--pristine', default=os.path.join(BUILD, 'mayfs.pristine.img'))
    ap.add_argument('--manifest', default=os.path.join(BUILD, 'mayfs_manifest.json'))
    ap.add_argument('--sectors', type=int, default=DEFAULT_SECTORS)
    ap.add_argument('--generation', type=lambda v: int(v, 0),
                    default=DEFAULT_GENERATION)
    ap.add_argument('--verify', action='store_true',
                    help='only verify the existing image at --out')
    args = ap.parse_args()

    if args.verify:
        checks, errors, info = verify_image(args.out)
        for name, cond in checks:
            print('  [%s] %s' % ('ok' if cond else 'FAIL', name))
        print('[mkfs_mayfs] checks=%d ok=%d fail=%d'
              % (len(checks), len(checks) - len(errors), len(errors)))
        if info:
            print('[mkfs_mayfs] generation=%d checksum=0x%08x image=%d bytes sha256=%s'
                  % (info['generation'], info['checksum'], info['image_bytes'],
                     info['image_sha256']))
        print('[mkfs_mayfs] VERIFY:', 'PASS' if not errors else 'FAIL')
        return 0 if not errors else 1

    os.makedirs(BUILD, exist_ok=True)
    img, table, slot = build_image(args.generation, args.sectors)
    with open(args.out, 'wb') as fh:
        fh.write(img)
    with open(args.pristine, 'wb') as fh:
        fh.write(img)
    print('[mkfs_mayfs] image  : %s (%d bytes = %d sectors)'
          % (args.out, len(img), len(img) // SECTOR))
    print('[mkfs_mayfs] layout : primary LBA 0..511, backup LBA 512..1023, '
          'table %d bytes, prefs @ %d' % (TABLE_BYTES, PREFS_OFFSET))
    print('[mkfs_mayfs] records: %d used of %d (fixed root dirs 0..3)'
          % (len(ENTRIES), MAX_FILES))
    print('[mkfs_mayfs] generation=%d table-checksum=0x%08x image-sha256=%s'
          % (args.generation, fnv1a32(table), hashlib.sha256(img).hexdigest()))
    print('[mkfs_mayfs] files  : ' + ', '.join(
        '%s(%d)' % (name, len(payload)) for name, _p, _d, payload in ENTRIES[4:]))

    manifest = {
        'image': os.path.basename(args.out),
        'bytes': len(img),
        'sectors': len(img) // SECTOR,
        'generation': args.generation,
        'table_checksum': fnv1a32(table),
        'image_sha256': hashlib.sha256(img).hexdigest(),
        'entries': [
            {'index': i, 'name': name, 'parent': parent, 'dir': is_dir,
             'size': len(payload), 'sha256': hashlib.sha256(payload).hexdigest()}
            for i, (name, parent, is_dir, payload) in enumerate(ENTRIES)
        ],
        'notes': ('MayFS: 512-sector primary slot at LBA 0, identical backup at '
                  'LBA 512, 48 x 4160-byte records, FNV-1a table checksum, '
                  'generation in the header. Largest legal file: 4095 bytes.'),
    }
    with open(args.manifest, 'w', encoding='utf-8') as fh:
        json.dump(manifest, fh, indent=2)
        fh.write('\n')
    print('[mkfs_mayfs] manifest: %s' % args.manifest)

    checks, errors, info = verify_image(args.out)
    print('[mkfs_mayfs] self-verify: checks=%d ok=%d fail=%d'
          % (len(checks), len(checks) - len(errors), len(errors)))
    if errors:
        for name, cond in checks:
            if not cond:
                print('  [FAIL] %s' % name)
        print('[mkfs_mayfs] RESULT: FAIL')
        return 1
    print('[mkfs_mayfs] RESULT: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
