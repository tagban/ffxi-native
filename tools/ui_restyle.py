"""A modern look for the game's interface pieces (window borders, corners, buttons, tags), drawn over
the originals pixel for pixel so every window keeps its layout: the texture's noise and wood grain
smoothed away, then its brightness mapped onto a modern palette - dark slate, cool silver edges - with
strongly coloured accents (gold, red) kept in their own hue but cleaned up. Alpha is kept as it is.

  python3 tools/ui_restyle.py --out <restyled pngs> <hash>_<w>x<h>.png ...

then upscale_texpack.py --model realesrgan-x4plus-anime --edge --grain 0 on the results makes the pack
(a ui-skin folder beside the profile's textures, loaded ahead of it). Needs numpy and pillow."""
import argparse
import os

import numpy as np
from PIL import Image, ImageFilter

# the palette, dark to light (RGB): slate panels, steel, cool silver edges
STOPS = np.array([[14, 17, 24], [34, 41, 54], [74, 86, 104], [150, 162, 180], [232, 238, 246]], np.float32)
POS = np.array([0.0, 0.22, 0.45, 0.72, 1.0], np.float32)


def palette(lum):
    out = np.empty(lum.shape + (3,), np.float32)
    for c in range(3):
        out[..., c] = np.interp(lum, POS, STOPS[:, c])
    return out


def restyle(rgba):
    rgb = rgba[:, :, :3].astype(np.float32)
    # noise and grain away, edges kept: a median, then a light blur where the texture is flat
    med = np.asarray(Image.fromarray(rgba[:, :, :3]).filter(ImageFilter.MedianFilter(3)), np.float32)
    soft = np.asarray(Image.fromarray(med.astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.8)), np.float32)
    edge = np.abs(med - soft).sum(2, keepdims=True)
    base = np.where(edge > 24, med, soft)
    lum = (base @ np.array([0.299, 0.587, 0.114], np.float32)) / 255.0
    # stretch the texture's own range over the palette, so a dim frame still reaches its silver
    lo, hi = np.percentile(lum, 2), np.percentile(lum, 98)
    t = np.clip((lum - lo) / max(1e-3, hi - lo), 0, 1)
    out = palette(t)
    # accents: a strongly coloured texel keeps its hue, brightened and cleaned
    mx, mn = base.max(2), base.min(2)
    sat = (mx - mn) / np.maximum(mx, 1.0)
    accent = np.clip((sat - 0.35) / 0.25, 0, 1)[..., None]
    hue = base / np.maximum(mx[..., None], 1.0)  # the colour at full brightness
    acc = hue * (60.0 + 190.0 * t[..., None])
    out = out * (1 - accent) + acc * accent
    return np.dstack([np.clip(out + 0.5, 0, 255).astype(np.uint8), rgba[:, :, 3]])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('pngs', nargs='+')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for p in a.pngs:
        rgba = np.array(Image.open(p).convert('RGBA'))
        Image.fromarray(restyle(rgba), 'RGBA').save(os.path.join(a.out, os.path.basename(p)))
    print(len(a.pngs), 'restyled')


if __name__ == '__main__':
    main()
