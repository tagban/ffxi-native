"""A zone's textures straight from the game's DAT files, as PNGs named by the fingerprint the renderer
gives them (d3d8.c texture_replacement: the FNV-1a hash of the first level as uploaded), so a texture
pack can be made for a zone without walking through it with FFXI_TEXDUMP.

  python3 tools/dat_textures.py --game <FINAL FANTASY XI folder> --out <pngs> --zone 106 [--zone 107 ...]
  python3 tools/dat_textures.py --game <folder> --out <pngs> --dat ROM/0/123.DAT

A zone's model DAT is file 100 + its number (zones below 256), found through VTABLE/FTABLE. Its images
are chunks of type 0x20: a flag byte, a 16-character name, a BITMAPINFOHEADER, and for the DXT ones
(flag 0xA1) the four-character code (stored backwards), the data's size, a word, and the blocks - the
same bytes the game uploads, so their hash is the renderer's. Other kinds (palettes the game turns
into 32-bit colour before uploading) are counted and left out.

Writes <hash>_<w>x<h>.png (texdump_png.py's names and layout) and names.txt (hash, size, the DAT's
name for the image). Needs numpy and pillow."""
import argparse
import os
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from texdump_png import dxt, FOURCC  # noqa: E402

DXT_FOURCC = {b'1TXD': FOURCC(b'DXT1'), b'3TXD': FOURCC(b'DXT3'), b'5TXD': FOURCC(b'DXT5')}


def dat_path(game, fid):
    vt = open(os.path.join(game, 'VTABLE.DAT'), 'rb').read()
    rom = vt[fid] if fid < len(vt) else 0
    if not rom:
        return None
    if rom == 1:
        ft, base = open(os.path.join(game, 'FTABLE.DAT'), 'rb').read(), 'ROM'
    else:
        ft, base = open(os.path.join(game, f'ROM{rom}', f'FTABLE{rom}.DAT'), 'rb').read(), f'ROM{rom}'
    v = struct.unpack_from('<H', ft, 2 * fid)[0]
    return os.path.join(game, base, str(v >> 7), f'{v & 127}.DAT')


def fnv1a(b):
    h = 0xcbf29ce484222325
    for x in b:  # (slow in Python, but textures are few)
        h = ((h ^ x) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def images(data):
    """(name, w, h, fourcc, level 0) of each DXT image chunk; the count of the others."""
    out, other, off = [], 0, 0
    while off + 16 <= len(data):
        word = struct.unpack_from('<I', data, off + 4)[0]
        kind, size = word & 0x7F, ((word >> 7) & 0x7FFFF) * 16
        if size < 16:
            break
        if kind == 0x20:
            c = off + 16
            flag = data[c]
            if flag == 0xA1 and c + 69 <= off + size:
                name = data[c + 1:c + 17].decode('latin-1').strip()
                w, h = struct.unpack_from('<ii', data, c + 21)
                cc = bytes(data[c + 57:c + 61])
                if cc in DXT_FOURCC and 0 < w <= 4096 and 0 < h <= 4096:
                    blk = 8 if cc == b'1TXD' else 16
                    n = ((w + 3) // 4) * ((h + 3) // 4) * blk
                    if c + 69 + n <= len(data):
                        out.append((name, w, h, DXT_FOURCC[cc], bytes(data[c + 69:c + 69 + n])))
                        off += size
                        continue
            other += 1
        off += size
    return out, other


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--zone', type=int, action='append', default=[])
    ap.add_argument('--dat', action='append', default=[])
    ap.add_argument('--min', type=int, default=64)
    a = ap.parse_args()
    dats = [os.path.join(a.game, d) for d in a.dat] + [p for p in (dat_path(a.game, 100 + z) for z in a.zone if z < 256) if p]
    os.makedirs(a.out, exist_ok=True)
    seen, names = set(), []
    for d in dats:
        found, other = images(open(d, 'rb').read())
        wrote = 0
        for name, w, h, fmt, lvl in found:
            if w < a.min and h < a.min:
                continue
            key = f'{fnv1a(lvl):016x}_{w}x{h}'
            if key in seen:
                continue
            seen.add(key)
            Image.fromarray(dxt(lvl, w, h, fmt), 'RGBA').save(os.path.join(a.out, key + '.png'))
            names.append(f'{key} {name} {os.path.relpath(d, a.game)}')
            wrote += 1
        print(f'{os.path.relpath(d, a.game)}: {len(found)} DXT images, {wrote} new written, {other} other kinds left out')
    with open(os.path.join(a.out, 'names.txt'), 'a') as f:
        f.write('\n'.join(names) + ('\n' if names else ''))


if __name__ == '__main__':
    main()
