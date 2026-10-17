"""The bullet holes (the levels' bullet hole decals: 1_1_aBulletHoles, 3_5_bulletholes) with relief: each hole a pit
with a raised, chipped rim, from the decal's own alpha; normal and material maps (roughness, height, cavity), so the
light catches their rims and their pits stay dark. Their class set to 'stone' with surface maps; materials.txt's
lines rewritten.

    python -I bulletholes.py <pack>
"""
import os
import sys
import tempfile

import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate  # noqa: E402

HOLES = ["56877efd0a4b4aa2", "cc6a72412d478004", "4dbff37cc46e0136"]


def blur(a, r):
    return np.asarray(Image.fromarray((np.clip(a, 0, 1) * 255).astype(np.uint8), "L").filter(
        ImageFilter.GaussianBlur(r))).astype(np.float32) / 255


def maps(img):
    rgba = np.asarray(img.convert("RGBA")).astype(np.float32) / 255
    a = rgba[..., 3]
    w = a.shape[1]
    # the pit: the hole's alpha, its centre deepest; the rim: a ring just outside it, chipped by noise
    core = blur(a, max(1, w / 256))
    ring = np.clip(blur(a, max(2, w / 64)) - core, 0, 1)
    rng = np.random.default_rng(7)
    chips = blur(rng.random(a.shape).astype(np.float32), max(1, w / 200))
    height = 0.5 - core * 0.5 + ring * 0.8 * (0.6 + 0.8 * chips)
    height = np.clip(height, 0, 1)
    # the normal from the height's slope
    gy, gx = np.gradient(height * (w / 32.0))
    n = np.dstack([-gx, gy, np.ones_like(height)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    normal = n * 0.5 + 0.5
    rough = np.full_like(height, 0.85)
    cavity = np.clip(1 - core * 0.7, 0, 1)
    spec = np.full_like(height, 0.05)
    return normal, np.dstack([rough, height, cavity, spec])


def main():
    pack = sys.argv[1]
    tex = os.path.join(pack, "textures")
    work = tempfile.mkdtemp(prefix="holes_")
    pngs = []
    for rid in HOLES:
        src = os.path.join(tex, rid + ".c.dds")
        if not os.path.exists(src):
            continue
        normal, mat = maps(Image.open(src))
        n = os.path.join(work, rid + ".n.png")
        m = os.path.join(work, rid + ".m.png")
        generate.save(n, normal, "RGB")
        generate.save(m, mat, "RGBA")
        pngs += [n, m]
    generate.texconv(pngs, "BC7_UNORM", tex)
    # materials.txt: a stone's surface (pbr), no parallax
    txt = os.path.join(pack, "materials.txt")
    out = []
    for line in open(txt).read().splitlines():
        rid = line.split(" ", 1)[0]
        if rid in HOLES:
            line = "%s stone 1 1.200 0.0000 0.050 -1 0" % rid
        out.append(line)
    open(txt, "w").write("\n".join(out) + "\n")
    print("bullet holes:", len(pngs) // 2)


if __name__ == "__main__":
    main()
