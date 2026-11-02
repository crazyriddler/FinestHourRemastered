"""A road laid as whole tiles in steps (level 30: its mud cells, a row shifted a cell where it turns), its outline
smoothed: a top view of where the tiles are (their pieces dumped with XBE_PIECES_NEAR + XBE_PIECES_OBJ), blurred and cut
a little past half by a smooth noise (fixed on the ground): the steps' corners cut round, the notches rounded, the long
edges an uneven line. The runtime fades the tiles out where it is 0 (renderer.cpp road mask, materials tiles 2).

Out: <pack>/roadmask_<level>.bin: "RMS2", width, height (uint32), x0, z0, the texel's size (float), the road's texture
drawn on its verges' cells inside the outline (uint64, the first of the textures), then the texels (uint8, rows by z).

    python tools/remaster/road_mask.py <near dir> <level> <texture,...> [--texel 32] [--blur 150]
"""
import argparse
import glob
import os
import struct

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("near")
    ap.add_argument("level", type=int)
    ap.add_argument("textures")
    ap.add_argument("--texel", type=float, default=32)
    ap.add_argument("--blur", type=float, default=150, help="units")
    ap.add_argument("--cut", type=float, default=0.6, help="the blurred cover kept past it")
    ap.add_argument("--pack", default=os.path.join(ROOT, "work", "remaster", "pack"))
    a = ap.parse_args()
    tex = set(a.textures.split(","))
    tris = []
    for f in glob.glob(os.path.join(a.near, "*.obj")):
        if os.path.basename(f).split("_")[0] not in tex:
            continue
        V, F = [], []
        for l in open(f):
            p = l.split()
            if not p:
                continue
            if p[0] == "v":
                V.append((float(p[1]), float(p[3])))
            elif p[0] == "f":
                F.append([int(x) - 1 for x in p[1:4]])
        tris += [[V[i] for i in t] for t in F]
    P = np.array([v for t in tris for v in t])
    pad = a.blur * 4
    x0, z0 = P[:, 0].min() - pad, P[:, 1].min() - pad
    w = int(np.ceil((P[:, 0].max() + pad - x0) / a.texel))
    h = int(np.ceil((P[:, 1].max() + pad - z0) / a.texel))
    img = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(img)
    for t in tris:
        d.polygon([((x - x0) / a.texel, (z - z0) / a.texel) for x, z in t], fill=255)
    cover = np.asarray(img).astype(np.float32) / 255
    blur = ndimage.gaussian_filter(cover, a.blur / a.texel)
    # the cut's noise: two scales, about 4 and 1.5 m (units of ~1 cm), fixed in the world
    rng = np.random.default_rng(30)
    def noise(scale):
        n = rng.random((h // max(1, int(scale / a.texel)) + 2, w // max(1, int(scale / a.texel)) + 2)).astype(np.float32)
        return np.asarray(Image.fromarray(n).resize((w, h), Image.BICUBIC))
    nz = noise(400) * 0.65 + noise(150) * 0.35
    cut = a.cut + (nz - 0.5) * 0.25
    mask = np.clip((blur - cut) / 0.08 + 0.5, 0, 1)
    out = os.path.join(a.pack, "roadmask_%d.bin" % a.level)
    with open(out, "wb") as f:
        f.write(b"RMS2" + struct.pack("<IIfffQ", w, h, x0, z0, a.texel, int(a.textures.split(",")[0], 16)))
        f.write((mask * 255 + 0.5).astype(np.uint8).tobytes())
    Image.fromarray((mask * 255).astype(np.uint8)).save(os.path.join(ROOT, "work", "road30", "mask_%d.png" % a.level))
    print("%s: %d x %d texels of %.0f units from %.0f %.0f, %d triangles" % (out, w, h, a.texel, x0, z0, len(tris)))


if __name__ == "__main__":
    main()
