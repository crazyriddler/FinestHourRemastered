"""The water textures (their names: water, river, the Volga's, sewers; not seats, boats or walls) as class 'water':
the runtime ripples their normals and reflects the scene in them (Water). Each gets flat surface maps when it has
none (the runtime's material path needs them), smooth and a little specular; materials.txt's lines rewritten.

    python -I water.py <extract folder> <pack>
"""
import json
import os
import sys
import tempfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate  # noqa: E402

WORDS = ["water", "river", "volgawater", "creek", "puddle", "ocean"]
NOT = ["seat", "boat", "wall", "edge", "drop", "splash", "refraction", "bubbles", "specularity", "ice", "soldier", "body",
       "debris", "hull", "canopy", "shield"]


def is_water(names):
    n = " ".join(names).lower()
    return any(w in n for w in WORDS) and not any(w in n for w in NOT)


def main():
    src, pack = sys.argv[1], sys.argv[2]
    idx = json.load(open(os.path.join(src, "index.json")))
    tex = os.path.join(pack, "textures")
    ids = [k for k, v in idx.items() if is_water(v["names"]) and os.path.exists(os.path.join(tex, k + ".c.dds"))]
    work = tempfile.mkdtemp(prefix="water_")
    pngs = []
    for rid in ids:
        w, h = Image.open(os.path.join(tex, rid + ".c.dds")).size
        n = np.dstack([np.full((h, w), 0.5), np.full((h, w), 0.5), np.ones((h, w))])
        m = np.dstack([np.full((h, w), 0.08), np.full((h, w), 0.5), np.ones((h, w)), np.full((h, w), 0.6)])
        pn, pm = os.path.join(work, rid + ".n.png"), os.path.join(work, rid + ".m.png")
        generate.save(pn, n, "RGB")
        generate.save(pm, m, "RGBA")
        pngs += [pn, pm]
    generate.texconv(pngs, "BC7_UNORM", tex)
    txt = os.path.join(pack, "materials.txt")
    out = []
    want = set(ids)
    for line in open(txt).read().splitlines():
        rid = line.split(" ", 1)[0]
        if rid in want:
            line = "%s water 1 0.500 0.0000 0.600 -1 1" % rid
        out.append(line)
    open(txt, "w").write("\n".join(out) + "\n")
    print("water:", len(ids), " ".join(sorted(" ".join(idx[k]["names"]) for k in ids))[:600])


if __name__ == "__main__":
    main()
