"""The tanks' vision slit glasses (drawn over the whole screen in a tank's view) again, smooth: the originals are
128 x 256 with their grime in a dithered or blocky alpha (the Sherman's in white blocks under a near clear alpha),
which stretched over the screen showed as blocks and speckles, and against the light as hard dark patches. The
grime melted by a wide blur (the colour premultiplied: no fringes from what is under a clear alpha), its contrast
halved so it reads only faintly, the rim's lines kept near sharp; brought up 4 times, BC7 with mips. Their
materials plain (no surface, no highlights, no grain: glass seen from behind, not a lit surface).

    python -I tank_glass.py <extract folder> <pack folder>
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate  # noqa: E402

# T34_glass_png, shermanGlass, glass_png and glass (the other tanks' slits)
GLASSES = ["3e4d009ca3efd1d1", "82bfdea4ed85ba86", "4ea09c068378be95", "f2b681a6b1d296d1"]
K = 4
SOFT = 6.0      # the grime's blur in the middle (texels of the 128 x 256 original)
RIM = 0.9       # the rim's
FAINT = 0.7     # how much of the grime's own variation is kept


def blur(a, sigma):
    """separable gaussian over the first two axes, edges clamped"""
    if sigma <= 0:
        return a
    r = int(np.ceil(sigma * 3))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    out = a
    for axis in (0, 1):
        pad = [(0, 0)] * a.ndim
        pad[axis] = (r, r)
        p = np.pad(out, pad, mode="edge")
        acc = np.zeros_like(out)
        for i, w in enumerate(k):
            sl = [slice(None)] * a.ndim
            sl[axis] = slice(i, i + out.shape[axis])
            acc += w * p[tuple(sl)]
        out = acc
    return out


def glass(src):
    h, w = src.shape[:2]
    a = src[..., 3:4] / 255.0
    pc = src[..., :3] * a  # premultiplied: the colour of what is there
    rgba = np.concatenate([pc, a], axis=2)
    # how far from the rim (texels): the rim's lines kept, the middle's grime melted
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    edge = np.minimum(np.minimum(xx, w - 1 - xx), np.minimum(yy, h - 1 - yy))
    inner = np.clip((edge - 3) / 8, 0, 1)[..., None]
    soft = blur(rgba, SOFT)
    # the grime faint: its variation about the glass's clean level (the middle's clearest tenth) halved, the colour
    # scaled with the alpha (premultiplied, the same tint)
    clean = np.percentile(soft[..., 3][inner[..., 0] > 0.99], 10)
    fa = clean + (soft[..., 3:4] - clean) * FAINT
    soft = np.concatenate([soft[..., :3] * (fa / np.maximum(soft[..., 3:4], 1e-4)), fa], axis=2)
    sharp = blur(rgba, RIM)
    rgba = sharp * (1 - inner) + soft * inner
    big = np.stack([np.asarray(Image.fromarray(rgba[..., c].astype(np.float32)).resize((w * K, h * K), Image.BICUBIC))
                    for c in range(4)], axis=2)
    alpha = np.clip(big[..., 3:4], 0, 1)
    col = np.where(alpha > 1e-4, big[..., :3] / np.maximum(alpha, 1e-4), 0)
    return np.concatenate([np.clip(col, 0, 255), alpha * 255], axis=2)


def main():
    src_dir, pack = sys.argv[1], sys.argv[2]
    work = os.path.join(pack, "work_misc")
    os.makedirs(work, exist_ok=True)
    files = []
    for g in GLASSES:
        src = np.asarray(Image.open(os.path.join(src_dir, g + ".png")).convert("RGBA")).astype(np.float32)
        out = glass(src)
        p = os.path.join(work, g + ".c.png")
        Image.fromarray((out + 0.5).astype(np.uint8), "RGBA").save(p)
        files.append(p)
        print(g, out.shape[1], "x", out.shape[0])
    generate.texconv(files, "BC7_UNORM", os.path.join(pack, "textures"))
    # materials.txt: plain, as the HUD's pieces (no surface maps read, no highlights, no grain)
    index = os.path.join(pack, "materials.txt")
    lines = [l for l in open(index).read().splitlines() if l.split(" ")[0] not in GLASSES]
    lines += ["%s ui 0 1.000 0.0000 0.150 -1 0" % g for g in GLASSES]
    open(index, "w").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
