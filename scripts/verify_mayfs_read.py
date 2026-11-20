#!/usr/bin/env python3
"""Independent host-side verifier for a MayFS volume image.

This parser is written from docs/mayfs-format-notes.md only: it does NOT import
the kernel driver and does NOT import scripts/mkfs_mayfs_testimg.py, so a bug in
the generator cannot hide a bug in the driver (or the other way round).  It
re-implements FNV-1a, the 48 x 4160-byte record table and the dual-slot header,
then proves:

  * the primary slot parses: magic, table checksum, MAY2 generation tag with its
    complement,
  * the backup slot parses the same way and carries the SAME generation and a
    byte-identical table (the commit wrote both slots from one snapshot),
  * the fixed root directories (records 0..3) and every parent chain are valid,
  * the fixture files are byte-exact (the port's ground truth),
  * optional --pristine <image>: every file that existed before the guest wrote
    is byte-identical afterwards, the only byte changes inside the snapshot are
    the header generation/checksum fields plus the new record's own 4160-byte
    slot - that is the "power-loss-safe commit did not damage old data" proof,
  * optional --expect-text NAME=TEXT: a file the guest created has exactly the
    expected payload (the shell `write` command appends one newline).

  py -3 scripts/verify_mayfs_read.py build/mayfs.img
  py -3 scripts/verify_mayfs_read.py build/mayfs.img \\
        --pristine build/mayfs.pristine.img --require-generation 8 \\
        --expect-text "NEW.TXT=hello mayfs"
"""
import argparse
import hashlib
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SECTOR = 512
SLOT_SECTORS = 512
SLOT_BYTES = SLOT_SECTORS * SECTOR
BACKUP_LBA = SLOT_SECTORS
MAX_FILES = 48
NAME_BYTES = 48
FILE_CAP = 4096
FILE_MAX = 4095
ENTRY_BYTES = 4160
TABLE_OFFSET = 512
TABLE_BYTES = MAX_FILES * ENTRY_BYTES
PREFS_OFFSET = 500 * SECTOR
PREFS_LENGTH = 32
MAGIC = b'MAYFS001'
GEN_TAG = b'MAY2'
PREFS_MAGIC = b'MAYPREF1'

# Independent restatement of the fixture (see docs/mayfs-format-notes.md).
FIXTURE = [
    ('HELLO.TXT', 0, False, b'Hello from MayFS.\nSecond line.\n'),
    ('BIG.BIN', 0, False, bytes(((i * 37 + 11) & 0xFF) for i in range(FILE_MAX))),
    ('SUBDIR', 0, True, b''),
    ('INNER.TXT', 6, False, b'inner file inside SUBDIR\n'),
    ('EMPTY', 0, False, b''),
    ('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCDEFG.TXT', 0, False,
     b'long name file\n'),
    ('ONE.TXT', 0, False, b'one\n'),
    ('DOC.TXT', 1, False, b'document inside Documents\n'),
]


def fnv1a32(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


class Slot:
    def __init__(self, raw):
        self.raw = raw
        self.errors = []
        self.generation = None
        self.checksum = None
        self.records = [None] * MAX_FILES
        self.prefs = None


def parse_slot(raw, label):
    s = Slot(raw)
    ok = True
    if raw[0:8] != MAGIC:
        s.errors.append('%s slot has no MAYFS001 magic (got %r)' % (label, raw[0:8]))
        return s, False
    s.checksum = struct.unpack_from('<I', raw, 8)[0]
    table = raw[TABLE_OFFSET:TABLE_OFFSET + TABLE_BYTES]
    if s.checksum != fnv1a32(table):
        s.errors.append('%s table checksum 0x%08x != stored 0x%08x'
                        % (label, fnv1a32(table), s.checksum))
        ok = False
    if raw[12:16] != GEN_TAG:
        s.errors.append('%s slot has no MAY2 generation tag' % label)
        return s, False
    s.generation = struct.unpack_from('<I', raw, 16)[0]
    if struct.unpack_from('<I', raw, 20)[0] != ((~s.generation) & 0xFFFFFFFF):
        s.errors.append('%s generation complement mismatch (gen=%d)'
                        % (label, s.generation))
        ok = False
    for i in range(MAX_FILES):
        off = TABLE_OFFSET + i * ENTRY_BYTES
        used, is_dir, parent, size = struct.unpack_from('<IIII', raw, off)
        if not used:
            continue
        name_raw = raw[off + 16:off + 64]
        data = raw[off + 64:off + 64 + FILE_CAP]
        if used != 1 or is_dir > 1 or parent >= MAX_FILES or size >= FILE_CAP:
            s.errors.append('%s record %d header invalid (used=%d dir=%d parent=%d '
                            'size=%d)' % (label, i, used, is_dir, parent, size))
            ok = False
            continue
        if name_raw[NAME_BYTES - 1] != 0:
            s.errors.append('%s record %d name is not NUL-terminated at 47'
                            % (label, i))
            ok = False
            continue
        if data[size] != 0:
            s.errors.append('%s record %d data[size=%d] is not 0' % (label, i, size))
            ok = False
        try:
            name = name_raw.split(b'\x00', 1)[0].decode('utf-8')
        except UnicodeDecodeError:
            s.errors.append('%s record %d name is not UTF-8' % (label, i))
            ok = False
            continue
        s.records[i] = {'index': i, 'name': name, 'dir': bool(is_dir),
                        'parent': parent, 'size': size, 'data': bytes(data[:size])}
    return s, ok


def check_root_dirs(slot, label, checks):
    for i in range(4):
        r = slot.records[i]
        checks.append(('%s: fixed root-dir record %d present, dir, parent=0'
                       % (label, i), bool(r) and r['dir'] and r['parent'] == 0))


def check_parents(slot, label, checks):
    ok = True
    for i in range(1, MAX_FILES):
        r = slot.records[i]
        if not r:
            continue
        p, depth = r['parent'], 0
        while p:
            if p >= MAX_FILES or not slot.records[p] or not slot.records[p]['dir']:
                ok = False
                break
            depth += 1
            if depth >= MAX_FILES:
                ok = False
                break
            p = slot.records[p]['parent']
        if not ok:
            break
    checks.append(('%s: every parent chain terminates at the root record' % label, ok))


def entry_map(slot):
    return {r['name']: r for r in slot.records if r}


def resolve(slot, path):
    """Walk a volume-relative path ('/SUBDIR/INNER.TXT') through parent records."""
    idx = 0
    for comp in [c for c in path.split('/') if c]:
        nxt = None
        for r in slot.records:
            if r and r['parent'] == idx and r['name'] == comp:
                nxt = r
                break
        if nxt is None:
            return None
        idx = nxt['index']
    return slot.records[idx]


def parse_expectations(args):
    """--expect-text NAME=TEXT  -> {name: bytes(TEXT + '\\n')}."""
    out = {}
    for spec in args.expect_text or []:
        if '=' not in spec:
            raise SystemExit('--expect-text wants NAME=TEXT, got %r' % spec)
        name, text = spec.split('=', 1)
        raw = text.encode('utf-8')
        if not raw.endswith(b'\n'):
            raw += b'\n'          # `write` always appends exactly one newline
        out[name] = raw
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image')
    ap.add_argument('--pristine', default=None,
                    help='image before the guest wrote: proves old files intact')
    ap.add_argument('--expect-text', action='append', default=[],
                    metavar='NAME=TEXT', help='file the guest created (newline added)')
    ap.add_argument('--require-generation', type=lambda v: int(v, 0), default=None)
    ap.add_argument('--json-out', default=None)
    args = ap.parse_args()

    with open(args.image, 'rb') as fh:
        img = fh.read()
    checks = []
    notes = []

    checks.append(('image is at least two 256 KiB slots (%d bytes)'
                   % len(img), len(img) >= 2 * SLOT_BYTES))
    checks.append(('image size is a whole number of 512-byte sectors',
                   len(img) % SECTOR == 0))
    primary, p_ok = parse_slot(img[:SLOT_BYTES], 'primary')
    backup, b_ok = parse_slot(img[BACKUP_LBA * SECTOR:BACKUP_LBA * SECTOR + SLOT_BYTES],
                              'backup')
    checks.append(('primary slot parses (magic + table FNV-1a checksum + MAY2 tag)',
                   p_ok))
    checks.append(('backup slot parses (magic + table FNV-1a checksum + MAY2 tag)',
                   b_ok))
    for err in primary.errors + backup.errors:
        notes.append(err)
    if not (p_ok and b_ok):
        _report(checks, notes, args, primary, backup)
        return 1

    both = img[:SLOT_BYTES] == img[BACKUP_LBA * SECTOR:BACKUP_LBA * SECTOR + SLOT_BYTES]
    checks.append(('both slots carry the same generation (%d == %d)'
                   % (primary.generation, backup.generation),
                   primary.generation == backup.generation))
    checks.append(('both slot tables are byte-identical',
                   primary.raw[TABLE_OFFSET:TABLE_OFFSET + TABLE_BYTES] ==
                   backup.raw[TABLE_OFFSET:TABLE_OFFSET + TABLE_BYTES]))
    checks.append(('both slot images are byte-identical (one committed snapshot '
                   'written to backup then primary)', both))
    # prefs record
    praw = primary.raw[PREFS_OFFSET:PREFS_OFFSET + 12 + PREFS_LENGTH]
    if praw[0:8] == PREFS_MAGIC:
        pc = struct.unpack_from('<I', primary.raw, PREFS_OFFSET + 8)[0]
        pdata = primary.raw[PREFS_OFFSET + 12:PREFS_OFFSET + 12 + PREFS_LENGTH]
        primary.prefs = bytes(pdata)
        checks.append(('32-byte prefs record at 500*512 has a valid FNV-1a checksum',
                       pc == fnv1a32(pdata)))
    else:
        checks.append(('prefs record at 500*512 is absent (legal: MayOS only writes '
                       'it once settings were saved)', True))

    check_root_dirs(primary, 'primary', checks)
    check_parents(primary, 'primary', checks)

    # ---- fixture ground truth ----
    ents = entry_map(primary)
    for name, parent, is_dir, payload in FIXTURE:
        r = ents.get(name)
        checks.append(('fixture %r present with type/parent/size %s/%d/%d'
                       % (name, 'dir' if is_dir else 'file', parent, len(payload)),
                       bool(r) and r['dir'] == is_dir and r['parent'] == parent
                       and r['size'] == len(payload)))
        if r:
            checks.append(('fixture %r payload byte-exact (sha256 %s)'
                           % (name, hashlib.sha256(payload).hexdigest()[:16]),
                           r['data'] == payload))
    checks.append(('BIG.BIN carries the largest legal MayFS file (4095 bytes)',
                   ents.get('BIG.BIN', {}).get('size') == FILE_MAX))
    checks.append(('a 47-character name is stored in the table',
                   any(len(r['name']) == 47 for r in primary.records if r)))
    nested = resolve(primary, '/SUBDIR/INNER.TXT')
    checks.append(('"/SUBDIR/INNER.TXT" resolves through the parent records',
                   nested is not None and nested['data'] == b'inner file inside SUBDIR\n'))
    checks.append(('EMPTY is a regular 0-byte file',
                   ents.get('EMPTY') is not None and not ents['EMPTY']['dir'] and
                   ents['EMPTY']['size'] == 0))

    # ---- guest-written files ----
    expect = parse_expectations(args)
    for name, want in expect.items():
        r = ents.get(name)
        checks.append(('guest file %r exists' % name, r is not None))
        if r:
            checks.append(('guest file %r payload is byte-exact %r'
                           % (name, want), r['data'] == want))
            checks.append(('guest file %r is a regular file' % name, not r['dir']))

    # ---- generation ----
    if args.require_generation is not None:
        checks.append(('generation == %d after the commit (found %d)'
                       % (args.require_generation, primary.generation),
                       primary.generation == args.require_generation))

    # ---- pristine comparison: old data untouched, only the new record moved ----
    pristine = None
    if args.pristine and os.path.exists(args.pristine):
        with open(args.pristine, 'rb') as fh:
            pimg = fh.read()
        pristine, pr_ok = parse_slot(pimg[:SLOT_BYTES], 'pristine-primary')
        checks.append(('pristine image parses', pr_ok))
        if pr_ok:
            checks.append(('generation advanced by exactly 1 (%d -> %d)'
                           % (pristine.generation, primary.generation),
                           primary.generation == pristine.generation + 1))
            old_ok = True
            for r in pristine.records:
                if not r:
                    continue
                now = primary.records[r['index']]
                if (not now or now['name'] != r['name'] or now['parent'] != r['parent']
                        or now['dir'] != r['dir'] or now['data'] != r['data']):
                    old_ok = False
                    notes.append('pre-existing record %d (%r) changed'
                                 % (r['index'], r['name']))
            checks.append(('every pre-existing record is unchanged (name/parent/'
                           'type/size/payload)', old_ok))

            # Record slots that were free before and are used now.
            added = [r for r in primary.records
                     if r and not pristine.records[r['index']]]
            expected_added = sorted(expect.keys())
            checks.append(('the filesystem gained exactly the expected record(s): %s'
                           % (', '.join(r['name'] for r in added) or 'none'),
                           sorted(r['name'] for r in added) == expected_added))
            added_start = min((r['index'] for r in added), default=None)
            added_end = max((r['index'] for r in added), default=None)
            allowed = set(range(8, 12)) | set(range(16, 24))   # checksum, generation
            if added_start is not None:
                lo = TABLE_OFFSET + added_start * ENTRY_BYTES
                hi = TABLE_OFFSET + (added_end + 1) * ENTRY_BYTES
                allowed |= set(range(lo, hi))
            pslot = pimg[:SLOT_BYTES]
            dslot = primary.raw
            stray = [i for i in range(SLOT_BYTES)
                     if pslot[i] != dslot[i] and i not in allowed]
            checks.append(('no byte outside the header counters and the new record '
                           'changed in the snapshot (%d stray bytes)'
                           % len(stray), not stray))
            changed = sum(1 for i in range(SLOT_BYTES) if pslot[i] != dslot[i])
            notes.append('pristine->dirty snapshot: %d of %d bytes differ '
                         '(generation + checksum + new record only)'
                         % (changed, SLOT_BYTES))
            if pristine.prefs is not None:
                checks.append(('prefs record preserved byte-exactly by the commit',
                               primary.prefs == pristine.prefs))
    elif args.pristine:
        notes.append('pristine image %r not found; comparison skipped'
                     % args.pristine)

    info = {
        'image': os.path.abspath(args.image),
        'bytes': len(img),
        'image_sha256': hashlib.sha256(img).hexdigest(),
        'generation': primary.generation,
        'table_checksum': primary.checksum,
        'used_records': sum(1 for r in primary.records if r),
        'slots_identical': both,
        'entries': [{'index': r['index'], 'name': r['name'], 'dir': r['dir'],
                     'parent': r['parent'], 'size': r['size'],
                     'sha256': hashlib.sha256(r['data']).hexdigest()}
                    for r in primary.records if r],
    }
    if args.json_out:
        with open(args.json_out, 'w', encoding='utf-8') as fh:
            json.dump(info, fh, indent=2)
            fh.write('\n')
    return _report(checks, notes, args, primary, backup, info)


def _report(checks, notes, args, primary, backup, info=None):
    failed = [n for n, c in checks if not c]
    for name, cond in checks:
        print('  [%s] %s' % ('ok' if cond else 'FAIL', name))
    for n in notes:
        print('  note: %s' % n)
    if info:
        print('[verify_mayfs] generation=%d checksum=0x%08x used=%d slots-identical=%s'
              % (info['generation'], info['table_checksum'], info['used_records'],
                 info['slots_identical']))
        print('[verify_mayfs] image sha256=%s' % info['image_sha256'])
    print('[verify_mayfs] checks=%d ok=%d fail=%d'
          % (len(checks), len(checks) - len(failed), len(failed)))
    print('[verify_mayfs] RESULT:', 'PASS' if not failed else 'FAIL')
    return 0 if not failed else 1


if __name__ == '__main__':
    sys.exit(main())
