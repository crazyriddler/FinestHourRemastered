"""Tiles of one ground laid side by side, each its own texture (level 30's road and verges: a grid of textures, one a
cell), their remastered maps made one by one: the colour, the normals and above all the relief's height jump where two
meet. Their maps stitched: along each side met by another tile, both shifted towards the same value at the seam (the
mean of their edges), the shift fading out over a band of the tile (its detail kept); the height matched by depth (the
two may have different parallax depths). The sides from tools/... adjacency (pairs: tile, side, tile, side, flipped,
count; L R T B: u at the tile's left or right edge, v at its top or bottom), the maps from a backup of their first
state, so it can be run again.

    python tools/remaster/seam_stitch.py <adjacency.json> [--band 0.12] [--min 10]
"""
import argparse
import collections
import json
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


def strip(img, side, i):
    """the row of texels i in from that side, along it (L R top to bottom, T B left to right)"""
    if side == "L":
        return img[:, i]
    if side == "R":
        return img[:, img.shape[1] - 1 - i]
    if side == "T":
        return img[i, :]
    return img[img.shape[0] - 1 - i, :]


def add_band(out, side, offset, band):
    """offset (along the side) added at the side, fading to nothing band texels in"""
    for i in range(band):
        w = (1 - i / band) ** 2
        if side == "L":
            out[:, i] += offset * w
        elif side == "R":
            out[:, out.shape[1] - 1 - i] += offset * w
        elif side == "T":
            out[i, :] += offset * w
        else:
            out[out.shape[0] - 1 - i, :] += offset * w


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("adjacency")
    ap.add_argument("--pack", default=os.path.join(ROOT, "work", "remaster", "pack"))
    ap.add_argument("--band", type=float, default=0.12, help="the band the shift fades over, a share of the side across")
    ap.add_argument("--min", type=int, default=10, help="pairs met fewer times than this left out (stray meetings)")
    a = ap.parse_args()
    tex = os.path.join(a.pack, "textures")
    backup = os.path.join(os.path.dirname(a.pack), "seam_backup")
    os.makedirs(backup, exist_ok=True)
    pom = {}
    for l in open(os.path.join(a.pack, "materials.txt")):
        p = l.split()
        if len(p) >= 5 and not l.startswith("#"):
            pom[p[0]] = float(p[4])
    pairs = [p for p in json.load(open(a.adjacency)) if p[5] >= a.min]
    ids = sorted({p[0] for p in pairs} | {p[2] for p in pairs})
    maps = {}
    for rid in ids:
        for kind in "cnm":
            src = os.path.join(tex, "%s.%s.dds" % (rid, kind))
            if os.path.exists(src) and not os.path.exists(os.path.join(backup, os.path.basename(src))):
                shutil.copy2(src, backup)
        maps[rid] = {k: np.asarray(Image.open(os.path.join(backup, "%s.%s.dds" % (rid, k))).convert("RGBA")).astype(np.float32)
                     for k in "cnm" if os.path.exists(os.path.join(backup, "%s.%s.dds" % (rid, k)))}

    def edge(rid, kind, side):
        """the side's own value along it: its two outer rows (the tile's coordinates stop half a texel or so in), the
        height as depth"""
        img = maps[rid][kind]
        e = (strip(img, side, 0) + strip(img, side, 1) + strip(img, side, 2)) / 3
        if kind == "m":
            e = e.copy()
            e[:, 1] = (255 - e[:, 1]) * pom.get(rid, 0.05)
        return e

    # each side's shift: towards the mean of its edge and its neighbours' (weighted by how often they meet)
    want = collections.defaultdict(lambda: [None, 0.0])
    for ta, sa, tb, sb, flip, n in pairs:
        for kind in "cnm":
            if kind not in maps[ta] or kind not in maps[tb]:
                continue
            ea, eb = edge(ta, kind, sa), edge(tb, kind, sb)
            if len(ea) != len(eb):
                continue
            if flip:
                eb = eb[::-1]
            mid = (ea + eb) / 2
            for rid, side, m_ in ((ta, sa, mid), (tb, sb, mid)):
                w = want[(rid, kind, side)]
                w[0] = m_ * n if w[0] is None else w[0] + m_ * n
                w[1] += n
    work = tempfile.mkdtemp(prefix="seam_")
    out = {"c": [], "m": [], "n": []}
    for rid in ids:
        for kind, img in maps[rid].items():
            res = img.copy()
            for side in "LRTB":
                w = want.get((rid, kind, side))
                if not w or w[0] is None:
                    continue
                target = w[0] / w[1]
                off = target - edge(rid, kind, side)
                if kind == "m":
                    off[:, 1] = -off[:, 1] / max(pom.get(rid, 0.05), 1e-4)  # (depth back to height)
                off[:, 3] = 0
                if kind == "n":
                    off[:, 2] = 0
                off = ndimage.gaussian_filter1d(off, 2.0, axis=0, mode="nearest")
                across = img.shape[1] if side in "LR" else img.shape[0]
                add_band(res, side, off, max(2, int(round(across * a.band))))
            png = os.path.join(work, "%s.%s.png" % (rid, kind))
            Image.fromarray(np.clip(res + 0.5, 0, 255).astype(np.uint8)).save(png)
            out[kind].append(png)
        print("%s: stitched" % rid)
    generate.texconv(out["c"] + out["m"], "BC7_UNORM", tex)
    generate.texconv(out["n"], "BC5_UNORM", tex)
    shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
