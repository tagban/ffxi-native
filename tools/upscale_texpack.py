"""Texture pack entries made by upscaling the game's own textures 4x with Real-ESRGAN.

  python3 tools/upscale_texpack.py --esrgan <realesrgan-ncnn-vulkan folder> --work <folder> --out <pack> \\
      <hash>_<w>x<h>.png ...

The PNGs are texdump_png.py's (a texture as the game uploads it). For each:

  - colour: upscaled by Real-ESRGAN (realesrgan-x4plus) with a border taken from the far side, as the
    texture repeats across the ground or a wall, so the edges of a tile meet without a seam; the
    border is cut off again after;
  - a bump map (its colour a direction: mostly (128, 128, 255)): resized smoothly instead and each
    texel made a direction again, as a network trained on photographs would make up detail in it;
  - alpha: resized smoothly (FFXI keeps a texture's opacity and other things there, 0x80 = opaque);

then make_texpack.py --alpha-data makes the entry (DXT5, every mipmap) in --out.

Needs numpy and pillow."""
import argparse
import os
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SCALE = 4


def is_bump(rgb):
    m = rgb.reshape(-1, 3).astype(np.float32).mean(0)
    return m[2] > 200 and abs(m[0] - 128) < 32 and abs(m[1] - 128) < 32


def resize(a, w, h):
    return np.array(Image.fromarray(a).resize((w, h), Image.LANCZOS))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--esrgan', required=True)
    ap.add_argument('--work', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--model', default='realesrgan-x4plus')
    ap.add_argument('pngs', nargs='+')
    a = ap.parse_args()
    src, dst, fin = (os.path.join(a.work, d) for d in ('in', 'up', 'final'))
    for d in (src, dst, fin):
        os.makedirs(d, exist_ok=True)

    jobs = []
    for p in a.pngs:
        name = os.path.basename(p)[:-4]
        rgba = np.array(Image.open(p).convert('RGBA'))
        h, w = rgba.shape[:2]
        bump = is_bump(rgba[:, :, :3])
        pad = max(4, min(w, h) // 8)
        if not bump:
            Image.fromarray(np.pad(rgba[:, :, :3], ((pad, pad), (pad, pad), (0, 0)), mode='wrap')).save(os.path.join(src, name + '.png'))
        jobs.append((name, rgba, w, h, pad, bump))

    if any(not j[5] for j in jobs):
        exe = os.path.join(a.esrgan, 'realesrgan-ncnn-vulkan')
        r = subprocess.run([exe, '-i', src, '-o', dst, '-n', a.model, '-s', str(SCALE), '-f', 'png',
                            '-m', os.path.join(a.esrgan, 'models')], capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stderr[-2000:])

    for name, rgba, w, h, pad, bump in jobs:
        W, H = w * SCALE, h * SCALE
        if bump:
            n = resize(rgba[:, :, :3], W, H).astype(np.float32) / 127.5 - 1.0
            n /= np.maximum(np.linalg.norm(n, axis=2, keepdims=True), 1e-6)
            rgb = np.clip((n + 1.0) * 127.5 + 0.5, 0, 255).astype(np.uint8)
        else:
            up = np.array(Image.open(os.path.join(dst, name + '.png')).convert('RGB'))
            o = pad * SCALE
            rgb = up[o:o + H, o:o + W]
        al = rgba[:, :, 3]
        alpha = np.full((H, W), al[0, 0], np.uint8) if al.min() == al.max() else resize(al, W, H)
        out = os.path.join(fin, name + '.png')
        Image.fromarray(np.dstack([rgb, alpha]), 'RGBA').save(out)
        hsh, size = name.split('_')
        r = subprocess.run([sys.executable, os.path.join(HERE, 'make_texpack.py'), '--hash', hsh, '--size', size,
                            '--png', out, '--out', a.out, '--alpha-data'], capture_output=True, text=True)
        print(r.stdout.strip() or r.stderr.strip(), '(bump map)' if bump else '')


if __name__ == '__main__':
    main()
