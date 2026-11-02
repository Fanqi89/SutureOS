#!/usr/bin/env python3
"""Build SutureOS' CJK bitmap font blob (16x16, GB2312 charset).

Font source: `ZhengGeDianHei-16.ttf` from the project collection
(HBOS/fonts). Its embedded `name` table declares:
    nameID 13: "This Font Software is licensed under the SIL Open Font
               License, Version 1.1."
    nameID 0 : "Copyright 2026 yzdnn, Copyright 2020 The DotGothic16 Project
               Authors" (derived from DotGothic16 / Fontworks)
so OFL-1.1 applies: embedding a rendered bitmap subset in a program is allowed
as long as the licence text and attribution travel with it. Both are in
docs/licenses/OFL-1.1.txt and docs/provenance.md.

Output format (`build/font_cjk.bin`), little endian:
    magic  'S','F','C','J'
    u32    count
    count x { u32 codepoint; u8 bitmap[32] }    sorted by codepoint
Each bitmap is 16 rows x 16 bits, most-significant bit = leftmost pixel.

Usage: py -3 scripts/mkfont_cjk.py [--ttf PATH] [--out build/font_cjk.bin]
"""
import argparse
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_TTF = os.path.join(os.path.dirname(ROOT), '自研操作系统', 'HBOS', 'fonts',
                           'ZhengGeDianHei-16.ttf')
OUT = os.path.join(ROOT, 'build', 'font_cjk.bin')
GLYPH_W, GLYPH_H = 16, 16


def gb2312_codepoints():
    cps = []
    for b1 in range(0xA1, 0xF8):
        for b2 in range(0xA1, 0xFF):
            try:
                ch = bytes((b1, b2)).decode('gb2312')
            except UnicodeDecodeError:
                continue
            cps.append(ord(ch))
    return cps


def render_glyphs(ttf, cps):
    from PIL import Image, ImageDraw, ImageFont

    font = ImageFont.truetype(ttf, 16)
    out = {}
    for cp in cps:
        img = Image.new('L', (GLYPH_W, GLYPH_H), 0)
        d = ImageDraw.Draw(img)
        try:
            d.text((GLYPH_W // 2, GLYPH_H // 2), chr(cp), font=font, fill=255,
                   anchor='mm')
        except Exception:                        # noqa: BLE001 - missing glyph
            continue
        data = bytearray(32)
        dark = 0
        for y in range(GLYPH_H):
            for x in range(GLYPH_W):
                if img.getpixel((x, y)) >= 96:
                    data[y * 2 + x // 8] |= 0x80 >> (x % 8)
                    dark += 1
        if dark:
            out[cp] = bytes(data)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ttf', default=DEFAULT_TTF)
    ap.add_argument('--out', default=OUT)
    args = ap.parse_args()
    if not os.path.exists(args.ttf):
        sys.exit('font not found: %s' % args.ttf)

    cps = gb2312_codepoints()
    glyphs = render_glyphs(args.ttf, cps)
    if ord('中') not in glyphs:
        sys.exit('sanity: 中 missing from the generated set')

    items = sorted(glyphs.items())
    blob = bytearray(b'SFCJ')
    blob += struct.pack('<I', len(items))
    for cp, data in items:
        blob += struct.pack('<I', cp)
        blob += data
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, 'wb') as fh:
        fh.write(bytes(blob))

    print('CJK font: %s (%d bytes, %d glyphs from %d GB2312 codepoints)'
          % (args.out, len(blob), len(items), len(cps)))
    print('  source: %s (SIL OFL-1.1, see docs/licenses/OFL-1.1.txt)'
          % args.ttf)
    g = glyphs[ord('中')]
    print('  glyph 中:')
    for y in range(GLYPH_H):
        print('    ' + ''.join('#' if (g[y * 2 + x // 8] >> (7 - x % 8)) & 1 else '.'
                               for x in range(GLYPH_W)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
