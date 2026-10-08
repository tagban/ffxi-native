"""The textures an FFXI_TEXDUMP run saved, as PNGs to look at and to upscale.

  FFXI_TEXLOG=1 FFXI_TEXDUMP=<dump> (the game, through the launcher)
  python3 tools/texdump_png.py --log <the profile's .log> --dump <dump> --out <pngs> [--min 64]
  python3 tools/texdump_png.py --log <texsave>/index.txt --dump <texsave> --out <pngs>
                                    (the launcher's "save new textures": FFXI_TEXSAVE)

Each dump is <hash>.bin, the first level of a texture as the game uploaded it (D3D's layout); its
size and format come from the log's "textures: WxH format F hash H" lines. Written as
<hash>_<w>x<h>.png (RGBA), the name make_texpack.py's --hash and --size take back. Textures smaller
than --min on both sides (icons, small effects) are left out.

Needs numpy and pillow."""
import argparse
import os
import re
import sys

import numpy as np
from PIL import Image

FOURCC = lambda s: s[0] | s[1] << 8 | s[2] << 16 | s[3] << 24
DXT1, DXT2, DXT3, DXT4, DXT5 = (FOURCC(b'DXT' + bytes([c])) for c in b'12345')


def c565(v):
    r = (v >> 11 & 31) * 255 // 31
    g = (v >> 5 & 63) * 255 // 63
    b = (v & 31) * 255 // 31
    return np.stack([r, g, b], -1).astype(np.int32)


def dxt(data, w, h, fmt):
    bw, bh = (w + 3) // 4, (h + 3) // 4
    blk = 8 if fmt == DXT1 else 16
    b = np.frombuffer(data[:bw * bh * blk], np.uint8).reshape(bh * bw, blk)
    col = b[:, -8:]
    c0 = col[:, 0].astype(np.int32) | col[:, 1].astype(np.int32) << 8
    c1 = col[:, 2].astype(np.int32) | col[:, 3].astype(np.int32) << 8
    p0, p1 = c565(c0), c565(c1)
    four = (c0 > c1) | (fmt != DXT1)
    pal = np.zeros((len(b), 4, 4), np.int32)
    pal[:, 0, :3], pal[:, 1, :3] = p0, p1
    pal[:, 2, :3] = np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    pal[:, 3, :3] = np.where(four[:, None], (p0 + 2 * p1) // 3, 0)
    pal[:, :, 3] = 255
    pal[:, 3, 3] = np.where(four, 255, 0)
    idx = col[:, 4:8].astype(np.uint32)
    bits = idx[:, 0] | idx[:, 1] << 8 | idx[:, 2] << 16 | idx[:, 3] << 24
    sel = (bits[:, None] >> (2 * np.arange(16, dtype=np.uint32))) & 3
    px = np.take_along_axis(pal, sel[:, :, None].astype(np.int64).repeat(4, 2), 1)  # n, 16, 4
    if fmt in (DXT2, DXT3):
        a = b[:, :8]
        nib = np.stack([a & 15, a >> 4], -1).reshape(len(b), 16)
        px[:, :, 3] = nib * 17
    elif fmt in (DXT4, DXT5):
        a0, a1 = b[:, 0].astype(np.int32), b[:, 1].astype(np.int32)
        ab = np.zeros(len(b), np.uint64)
        for i in range(6):
            ab |= b[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
        ai = (ab[:, None] >> (3 * np.arange(16, dtype=np.uint64))) & np.uint64(7)
        ai = ai.astype(np.int32)
        apal = np.zeros((len(b), 8), np.int32)
        apal[:, 0], apal[:, 1] = a0, a1
        six = a0 > a1
        for i in range(1, 7):
            apal[:, i + 1] = np.where(six, ((7 - i) * a0 + i * a1) // 7, 0)
        for i in range(1, 5):
            apal[:, i + 1] = np.where(six, apal[:, i + 1], ((5 - i) * a0 + i * a1) // 5)
        apal[:, 6] = np.where(six, apal[:, 6], 0)
        apal[:, 7] = np.where(six, apal[:, 7], 255)
        px[:, :, 3] = np.take_along_axis(apal, ai, 1)
    img = px.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return img[:h, :w].astype(np.uint8)


def plain(data, w, h, fmt):
    if fmt in (21, 22):  # A8R8G8B8, X8R8G8B8
        a = np.frombuffer(data[:w * h * 4], np.uint8).reshape(h, w, 4)
        out = a[:, :, [2, 1, 0, 3]].copy()
        if fmt == 22:
            out[:, :, 3] = 255
        return out
    v = np.frombuffer(data[:w * h * 2], '<u2').reshape(h, w).astype(np.int32)
    if fmt == 23:  # R5G6B5
        rgb = c565(v)
        return np.dstack([rgb, np.full((h, w), 255)]).astype(np.uint8)
    if fmt in (24, 25):  # X1R5G5B5, A1R5G5B5
        r, g, b = (v >> 10 & 31) * 255 // 31, (v >> 5 & 31) * 255 // 31, (v & 31) * 255 // 31
        a = np.where(v >> 15, 255, 0) if fmt == 25 else np.full((h, w), 255)
        return np.dstack([r, g, b, a]).astype(np.uint8)
    if fmt == 26:  # A4R4G4B4
        return np.dstack([(v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17, (v >> 12 & 15) * 17]).astype(np.uint8)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--log', required=True)
    ap.add_argument('--dump', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--min', type=int, default=64)
    a = ap.parse_args()
    seen = {}
    pat = re.compile(r'textures: (\d+)x(\d+) format ([0-9a-f]+) hash ([0-9a-f]{16})')
    for line in open(a.log, errors='replace'):
        m = pat.search(line)
        if m:
            seen[m.group(4)] = (int(m.group(1)), int(m.group(2)), int(m.group(3), 16))
    os.makedirs(a.out, exist_ok=True)
    done = skipped = 0
    for hsh, (w, h, fmt) in sorted(seen.items()):
        path = os.path.join(a.dump, hsh + '.bin')
        if not os.path.exists(path):  # the launcher's "save new textures" (FFXI_TEXSAVE) names them so
            path = os.path.join(a.dump, f'{hsh}_{w}x{h}.bin')
        if (w < a.min and h < a.min) or not os.path.exists(path):
            skipped += 1
            continue
        data = open(path, 'rb').read()
        img = dxt(data, w, h, fmt) if fmt in (DXT1, DXT2, DXT3, DXT4, DXT5) else plain(data, w, h, fmt)
        if img is None:
            print(f'{hsh}: format {fmt:08x} not read', file=sys.stderr)
            skipped += 1
            continue
        Image.fromarray(img, 'RGBA').save(os.path.join(a.out, f'{hsh}_{w}x{h}.png'))
        done += 1
    print(f'{done} written, {skipped} left out, of {len(seen)} in the log')


if __name__ == '__main__':
    main()
