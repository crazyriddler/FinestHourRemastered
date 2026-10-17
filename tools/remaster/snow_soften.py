"""The snowy ground's colour textures (the pack's snow.txt) softened: the upscaler drew the original's faint grey specks
as thin dark cracks, which the lighting turned into dark scratches over all the snow. The fine detail darker than its
surroundings is kept at a third, the lighter kept; the broad shading as it was. The tracks' mud listed too (the
original dark, mean 170 or less) softer still: laid over the snow see-through, its sharp cracks showed as black
scratches. The textures before are kept in <pack>/../snow_backup.

    python -I snow_soften.py <pack> [--keep 0.33]
"""
import argparse
import os
import shutil
import sys
import tempfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate  # noqa: E402
from alpha_clean import blur  # noqa: E402


def whiten(rgb, gain):
    """the snow whiter: its tint pulled halfway to grey (a faint green), brighter, the highlights kept under white"""
    lum = rgb.mean(axis=2, keepdims=True)
    rgb = lum + (rgb - lum) * 0.5
    return 255 - (255 - rgb) / gain


def soften(rgba, keep, gain=1.0):
    a = rgba.astype(np.float32)
    rgb = a[..., :3]
    if gain != 1.0:
        rgb = whiten(rgb, gain)
    base = blur(rgb, 3.0)  # (the broad shading: kept)
    hp = rgb - base
    lum = hp.mean(axis=2, keepdims=True)
    # darker than about: a third of it; lighter: kept
    hp = np.where(lum < 0, hp * keep, hp)
    out = a.copy()
    out[..., :3] = np.clip(base + hp, 0, 255)
    return out.astype(np.uint8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pack")
    ap.add_argument("--keep", type=float, default=0.33)
    ap.add_argument("--white", type=float, default=1.35, help="the snow's white: its darkness under white divided by this")
    ap.add_argument("--mud", type=float, default=0.3, help="the tracks' mud (laid over the snow, see-through): its dark detail kept at this")
    a = ap.parse_args()
    tex = os.path.join(a.pack, "textures")
    extract = os.path.join(os.path.dirname(os.path.abspath(a.pack)), "extract")
    backup = os.path.join(os.path.dirname(os.path.abspath(a.pack)), "snow_backup")
    os.makedirs(backup, exist_ok=True)
    ids = [l.strip() for l in open(os.path.join(a.pack, "snow.txt")) if l.strip() and not l.startswith("#")]
    work = tempfile.mkdtemp(prefix="snow_soften_")
    pngs = []
    for rid in ids:
        src = os.path.join(tex, rid + ".c.dds")
        orig = os.path.join(extract, rid + ".png")
        if not os.path.exists(src) or not os.path.exists(orig):
            continue
        # the tracks' mud, laid over the snow see-through: its cracks, sharp in the upscale, showed through as
        # black scratches (the original's blur tints the snow grey alone)
        mud = np.asarray(Image.open(orig).convert("RGB")).astype(np.float32).mean() <= 170
        kept = os.path.join(backup, rid + ".c.dds")
        if not os.path.exists(kept):
            shutil.copy2(src, kept)
        rgba = np.asarray(Image.open(kept).convert("RGBA"))  # from the pack's own, again
        png = os.path.join(work, rid + ".c.png")
        Image.fromarray(soften(rgba, a.mud if mud else a.keep, 1.0 if mud else a.white), "RGBA").save(png)
        pngs.append(png)
    print(len(pngs), "snow textures softened", flush=True)
    generate.texconv(pngs, "BC7_UNORM", tex)
    shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
