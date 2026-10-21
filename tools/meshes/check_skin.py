"""The remastered skinned meshes checked against their originals, posed as the game posed them when they were dumped
(the dump's vertex constants: each bone's three rows at c[index + 14..16], RenderWare's skin program): how far the
remastered mesh's vertices land from the original's posed vertices. A broken mesh (bones mixed up, a spike) shows
as a vertex far from any original one.

    python tools/meshes/check_skin.py [--dump work/meshes/all] [--pack work/remaster/pack/meshes] [--worst 20]
"""
import argparse
import glob
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fhm  # noqa: E402


def posed(m, consts):
    """the vertices skinned by the dump's bones (registers 1 weights, 2 indices x3)"""
    p = np.concatenate([m.reg(0)[:, :3], np.ones((len(m.vertices), 1))], 1)
    w, idx = m.reg(1)[:, :3], m.reg(2)[:, :3].astype(int)
    out = np.zeros((len(p), 3))
    for k in range(3):
        base = idx[:, k] + 14 + 96
        rows = np.stack([consts[np.clip(base + r, 0, 191)] for r in range(3)], 1)  # [n, 3, 4]
        out += w[:, k:k + 1] * np.einsum("nrc,nc->nr", rows, p)
    return out


def main():
    ap = argparse.ArgumentParser()
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--dump", default=os.path.join(root, "work", "meshes", "all"))
    ap.add_argument("--pack", default=os.path.join(root, "work", "remaster", "pack", "meshes"))
    ap.add_argument("--worst", type=int, default=20)
    a = ap.parse_args()
    results = []
    for path in glob.glob(os.path.join(a.pack, "*.fhm")):
        key = os.path.basename(path)
        src_path = os.path.join(a.dump, key)
        if not os.path.exists(src_path):
            continue
        src = fhm.read(src_path)
        if src.constants is None or 1 not in src.reg_list() or 2 not in src.reg_list():
            continue
        if np.abs(src.reg(2)[:, :3] - np.round(src.reg(2)[:, :3])).max() > 1e-4:
            continue
        new = fhm.read(path)
        a0, b0 = posed(src, src.constants), posed(new, src.constants)
        # each new vertex's distance to the nearest original one (posed), against the mesh's size
        size = float(np.linalg.norm(a0.max(0) - a0.min(0))) or 1.0
        far = 0.0
        for i in range(0, len(b0), 2048):
            d = np.linalg.norm(b0[i:i + 2048, None, :] - a0[None, :, :], axis=2).min(1)
            far = max(far, float(d.max()))
        results.append((far / size, far, key, len(src.vertices)))
    results.sort(reverse=True)
    for r, far, key, n in results[:a.worst]:
        print(f"{key} {n} vertices: farthest {far:.1f} ({r:.2f} of its size)")
    print(f"{len(results)} skinned meshes checked; {sum(1 for r in results if r[0] > 0.15)} past 0.15 of their size")


if __name__ == "__main__":
    main()
