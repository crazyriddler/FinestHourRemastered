"""The grey between a remastered texture's islands filled with the islands' own colours (each pixel the nearest one
inside), so a sample at an island's edge (its filtering, its smaller mips) does not take the grey in: the dark line along
the first-person thumb, where its island meets the palm's. The grey: unsaturated, mid-light, with the 2 pixels about it
(the islands' edges mixed with it); the colour map (and the material map with the same mask) compressed again (BC7, mips).
The textures before are kept in <pack>/../pad_backup. --checker: the artists' blue and white checker between the
islands in place of the grey (uniforms: a sleeve's seam showed a blue and white stripe, issue #49): bluish pixels in
two-toned patches.

    python tools/remaster/pad_islands.py <id>[,<id>...] [--pack work/remaster/pack] [--checker]
"""
import argparse
import os
import shutil
import sys
import tempfile

import numpy as np
from PIL import Image
from scipy import ndimage

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def checker(rgb):
    """The checker: bluish pixels (blue well over red and green, those two alike), in patches of two tones."""
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    blue = (b - np.maximum(r, g) > 25) & (np.abs(r - g) < 30)
    closed = ndimage.binary_closing(blue, iterations=1) | blue
    lab, n = ndimage.label(closed)
    if n == 0:
        return np.zeros_like(blue)
    idx = np.arange(1, n + 1)
    area = ndimage.sum(np.ones_like(blue), lab, idx)
    lum = rgb.mean(2)
    mean = ndimage.mean(lum, lab, idx)
    std = np.sqrt(np.maximum(ndimage.mean(lum * lum, lab, idx) - mean * mean, 0))
    strict = np.isin(lab, idx[(area >= 16) & (std > 12)])
    # (and the paler squares and the islands' edges blended with them, faintly bluish, beside it)
    soft = (b - np.maximum(r, g) > 8) & (np.abs(r - g) < 30)
    near = ndimage.binary_dilation(strict, iterations=6)
    return strict | (soft & near)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ids")
    ap.add_argument("--pack", default=os.path.join(ROOT, "work", "remaster", "pack"))
    ap.add_argument("--grow", type=int, default=2, help="pixels about the grey taken too (an outline drawn round the islands)")
    ap.add_argument("--checker", action="store_true", help="the blue and white checker between the islands, not the grey")
    a = ap.parse_args()
    tex = os.path.join(a.pack, "textures")
    backup = os.path.join(os.path.dirname(a.pack), "pad_backup")
    os.makedirs(backup, exist_ok=True)
    work = tempfile.mkdtemp(prefix="pad_")
    pngs = []
    for rid in a.ids.split(","):
        cpath = os.path.join(tex, rid + ".c.dds")
        for kind in ("c", "m"):
            p = os.path.join(tex, "%s.%s.dds" % (rid, kind))
            if os.path.exists(p) and not os.path.exists(os.path.join(backup, os.path.basename(p))):
                shutil.copy2(p, backup)
        c = np.asarray(Image.open(os.path.join(backup, rid + ".c.dds")).convert("RGBA")).copy()
        rgb = c[..., :3].astype(int)
        grey = checker(rgb) if a.checker else (rgb.max(2) - rgb.min(2) < 14) & (rgb.mean(2) > 90) & (rgb.mean(2) < 200)
        mask = ndimage.binary_dilation(grey, iterations=a.grow)
        _, (iy, ix) = ndimage.distance_transform_edt(mask, return_indices=True)
        for kind in ("c", "m"):
            src = os.path.join(backup, "%s.%s.dds" % (rid, kind))
            if not os.path.exists(src):
                continue
            img = np.asarray(Image.open(src).convert("RGBA")).copy()
            if img.shape[:2] != mask.shape:
                continue
            img[mask] = img[iy[mask], ix[mask]]
            png = os.path.join(work, "%s.%s.png" % (rid, kind))
            Image.fromarray(img).save(png)
            pngs.append(png)
        print("%s: %d grey pixels, %d filled" % (rid, grey.sum(), mask.sum()))
    generate.texconv(pngs, "BC7_UNORM", tex)
    shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
