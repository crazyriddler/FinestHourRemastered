"""The outline a remastered texture drew along its islands' edges taken out where the model's seams are: the dark line
along the first-person thumb (the Russian arms', the thumb's island against the hand's back, no grey between them for
pad_islands.py to find). The original model gives the seams in the texture: the edges of its triangles that only one
triangle has (a seam splits the vertices). A band of `width` pixels each side of them, and the grey between the islands
(as pad_islands.py), is filled from inside: each band pixel the nearest pixel of its own original triangle beyond the
band (not the island across the seam), a pixel of none the nearest outside the band. The colour, material and normal maps,
from the textures before (<pack>/../pad_backup), compressed again (BC7, the normals BC5, mips).

    python tools/remaster/seam_outline.py <texture id> <mesh key> [--width 4] [--pack work/remaster/pack]
"""
import argparse
import os
import shutil
import sys
import tempfile

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "meshes"))
import fhm  # noqa: E402
import generate  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tex")
    ap.add_argument("mesh")
    ap.add_argument("--width", type=int, default=4)
    ap.add_argument("--pack", default=os.path.join(ROOT, "work", "remaster", "pack"))
    a = ap.parse_args()
    tex = os.path.join(a.pack, "textures")
    backup = os.path.join(os.path.dirname(a.pack), "pad_backup")
    os.makedirs(backup, exist_ok=True)
    for kind in ("c", "m", "n"):
        p = os.path.join(tex, "%s.%s.dds" % (a.tex, kind))
        if os.path.exists(p) and not os.path.exists(os.path.join(backup, os.path.basename(p))):
            shutil.copy2(p, backup)
    c = np.asarray(Image.open(os.path.join(backup, a.tex + ".c.dds")).convert("RGBA"))
    H, W = c.shape[:2]
    o = fhm.read(os.path.join(ROOT, "work", "meshes", "all", a.mesh + ".fhm"))
    uvr = 9 if o.regs >> 9 & 1 else 5
    uv = o.reg(uvr)[:, :2].astype(float) * [W, H]
    ot = o.indices.astype(np.int64)
    # each pixel's original triangle (-1 none)
    ids = Image.new("I", (W, H), -1)
    d = ImageDraw.Draw(ids)
    for i, tri in enumerate(ot):
        d.polygon([tuple(uv[v]) for v in tri], fill=i)
    tid = np.asarray(ids)
    # the seams: edges one triangle alone has
    count = {}
    for tri in ot:
        for x, y in ((0, 1), (1, 2), (2, 0)):
            k = (min(tri[x], tri[y]), max(tri[x], tri[y]))
            count[k] = count.get(k, 0) + 1
    band_img = Image.new("L", (W, H), 0)
    bd = ImageDraw.Draw(band_img)
    seams = [k for k, n in count.items() if n == 1]
    for v0, v1 in seams:
        bd.line([tuple(uv[v0]), tuple(uv[v1])], fill=255, width=2 * a.width)
    band = np.asarray(band_img) > 0
    rgb = c[..., :3].astype(int)
    grey = (rgb.max(2) - rgb.min(2) < 14) & (rgb.mean(2) > 90) & (rgb.mean(2) < 200)
    mask = band | ndimage.binary_dilation(grey, iterations=a.width)
    # the remastered islands do not lie on the original's to the pixel: about the seams (3 bands' width), the outline
    # itself too, the pixels well darker than the skin about them (a thin line under a 9-pixel median), grown by one
    near = ndimage.binary_dilation(band, iterations=2 * a.width)
    lum = rgb.mean(2)
    med = ndimage.median_filter(lum, size=9)
    dark = near & (lum < med * 0.82) & (med - lum > 10)
    mask |= ndimage.binary_dilation(dark, iterations=1)
    # the fill's source for each masked pixel: its own triangle's nearest unmasked pixel, else the nearest unmasked
    _, (gy, gx) = ndimage.distance_transform_edt(mask, return_indices=True)
    sy, sx = gy.copy(), gx.copy()
    for t in np.unique(tid[mask]):
        if t < 0:
            continue
        region = tid == t
        need = region & mask
        core = region & ~mask
        if not core.any() or not need.any():
            continue
        ys, xs = np.nonzero(region)
        y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
        _, (ly, lx) = ndimage.distance_transform_edt(~core[y0:y1, x0:x1], return_indices=True)
        ny, nx = np.nonzero(need[y0:y1, x0:x1])
        sy[ny + y0, nx + x0] = ly[ny, nx] + y0
        sx[ny + y0, nx + x0] = lx[ny, nx] + x0
    work = tempfile.mkdtemp(prefix="seam_")
    pngs, normals = [], []
    for kind in ("c", "m", "n"):
        src = os.path.join(backup, "%s.%s.dds" % (a.tex, kind))
        if not os.path.exists(src):
            continue
        img = np.asarray(Image.open(src).convert("RGBA")).copy()
        if img.shape[:2] != mask.shape:
            continue
        img[mask] = img[sy[mask], sx[mask]]
        if kind == "n":
            img[mask, :2] = 128  # (flat there: a seam's ridge in the normals shaded a dark line)
        png = os.path.join(work, "%s.%s.png" % (a.tex, kind))
        Image.fromarray(img).save(png)
        (normals if kind == "n" else pngs).append(png)
    print("%s: %d seam edges, %d pixels filled" % (a.tex, len(seams), mask.sum()))
    generate.texconv(pngs, "BC7_UNORM", tex)
    if normals:
        generate.texconv(normals, "BC5_UNORM", tex)  # (the normal map: its seams' edges shaded a dark line too)
    shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
