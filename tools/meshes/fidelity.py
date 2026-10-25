"""How far each remastered mesh (the pack's) strays from the game's own (the dump's): points spread over each surface,
for every one the distance to the nearest point of the other. "uncovered" is the share of the original's surface
with no remastered surface near it (holes, cracks, a piece moved off), "off" the share of the remastered surface far
from the original's (bulges, folds, a building bent), "hole" the area (and widest point) of the original's surface
the remastered one leaves open: uncovered where the remastered surface ends in an open edge (a crack the background
shows through), not where it rounds a corner. Worst first.

    python tools/meshes/fidelity.py [--dump work/meshes/all] [--pack work/remaster/pack/meshes] [--only key,key]
        [--tol 1.5] [--csv out.csv]
"""
import argparse
import concurrent.futures
import glob
import os
import sys

import numpy as np
from scipy.spatial import cKDTree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fhm  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MAX_POINTS = 3000000  # a surface's points at most (its spacing widened past this)


def triangles(m):
    p = m.reg(0)[:, :3].astype(np.float64)
    t = np.asarray(m.indices).reshape(-1, 3)
    a, b, c = p[t[:, 0]], p[t[:, 1]], p[t[:, 2]]
    area = 0.5 * np.linalg.norm(np.cross(b - a, c - a), axis=1)
    keep = area > 1e-9
    return a[keep], b[keep], c[keep], area[keep]


def spread(tri, spacing, rng):
    """points over the triangles about spacing apart (by area), the corners and the edges' middles as well"""
    a, b, c, area = tri
    n = np.maximum(1, np.round(area / (spacing * spacing)).astype(np.int64))
    idx = np.repeat(np.arange(len(a)), n)
    u, v = rng.random(len(idx)), rng.random(len(idx))
    flip = u + v > 1
    u[flip], v[flip] = 1 - u[flip], 1 - v[flip]
    pts = a[idx] + (b[idx] - a[idx]) * u[:, None] + (c[idx] - a[idx]) * v[:, None]
    return np.concatenate([pts, a, b, c, (a + b) / 2, (b + c) / 2, (c + a) / 2])


def open_edges(m):
    """the mesh's open edges (one triangle only, corners at the same place joined) as point pairs"""
    p = m.reg(0)[:, :3].astype(np.float64)
    _, wid = np.unique(np.round(p, 3), axis=0, return_inverse=True)
    wid = wid.ravel()
    P = np.zeros((wid.max() + 1, 3))
    P[wid] = p
    t = wid[np.asarray(m.indices).reshape(-1, 3)]
    t = t[(t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])]
    e = np.sort(np.concatenate([t[:, [0, 1]], t[:, [1, 2]], t[:, [2, 0]]]), axis=1)
    u, n = np.unique(e, axis=0, return_counts=True)
    u = u[n == 1]
    return P[u[:, 0]], P[u[:, 1]]


def edge_points(ea, eb, step):
    """points along the edges step apart (the ends too)"""
    if not len(ea):
        return np.zeros((0, 3))
    L = np.linalg.norm(eb - ea, axis=1)
    k = np.maximum(1, np.ceil(L / step).astype(np.int64))
    idx = np.repeat(np.arange(len(ea)), k + 1)
    f = np.concatenate([np.linspace(0, 1, int(c) + 1) for c in k])
    return ea[idx] + (eb[idx] - ea[idx]) * f[:, None]


def compare(orig, new, tol):
    to, tn = triangles(orig), triangles(new)
    if not len(to[0]) or not len(tn[0]):
        return None
    total = max(to[3].sum(), tn[3].sum())
    spacing = max(0.5, np.sqrt(total / MAX_POINTS))
    rng = np.random.default_rng(1)
    po, pn = spread(to, spacing, rng), spread(tn, spacing, rng)
    # (a point on a surface is at most about a spacing from the other's points when the surfaces are the same)
    limit = tol + 1.5 * spacing
    do, near = cKDTree(pn).query(po, distance_upper_bound=limit * 8)
    dn, _ = cKDTree(po).query(pn, distance_upper_bound=limit * 8)
    do, dn = np.minimum(do, limit * 8), np.minimum(dn, limit * 8)
    size = float(np.linalg.norm(po.max(0) - po.min(0)))
    # a hole: a point of the original's surface (not at its own open edges) with no remastered surface near it, the
    # remastered surface nearest it ending in an open edge there (rounding a corner leaves the original's corner
    # uncovered too, the surface nearest it whole)
    hole = np.zeros(len(po), bool)
    far = do > limit
    if far.any():
        ne = edge_points(*open_edges(new), spacing / 2)
        oe = edge_points(*open_edges(orig), spacing / 2)
        if len(ne):
            q = po[far]
            j = np.minimum(near[far], len(pn) - 1)
            at_edge = cKDTree(ne).query(pn[j])[0] < 1.5 * spacing
            own = cKDTree(oe).query(q)[0] > limit if len(oe) else np.ones(len(q), bool)
            hole[np.flatnonzero(far)[at_edge & own]] = True
    n_random = len(po) - 6 * len(to[0])  # (the spread points, one about each spacing squared)
    hole_area = float(hole[:n_random].sum()) * spacing * spacing
    return {"hole_area": hole_area, "hole_width": float(do[hole].max()) if hole.any() else 0.0,
            "uncovered": float(far.mean()), "off": float((dn > limit).mean()),
            "worst": float(max(do.max(), dn.max())), "spacing": float(spacing), "size": size}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", default=os.path.join(ROOT, "work", "meshes", "all"))
    ap.add_argument("--pack", default=os.path.join(ROOT, "work", "remaster", "pack", "meshes"))
    ap.add_argument("--only", default="")
    ap.add_argument("--tol", type=float, default=1.5, help="distance (the game's units) still the same surface")
    ap.add_argument("--csv", default="")
    ap.add_argument("--jobs", type=int, default=12)
    a = ap.parse_args()
    only = {k for k in a.only.split(",") if k}
    keys = [os.path.splitext(os.path.basename(p))[0] for p in sorted(glob.glob(os.path.join(a.pack, "*.fhm")))]
    keys = [k for k in keys if (not only or k in only) and os.path.exists(os.path.join(a.dump, k + ".fhm"))]

    def one(k):
        try:
            o, n = fhm.read(os.path.join(a.dump, k + ".fhm")), fhm.read(os.path.join(a.pack, k + ".fhm"))
            r = compare(o, n, a.tol)
            if r:
                r.update(key=k, tex="%016x" % o.tex, verts=len(o.vertices))
            return r
        except Exception as e:  # noqa: BLE001
            return {"key": k, "error": str(e)}

    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        rows = [r for r in pool.map(one, keys) if r]
    errs = [r for r in rows if "error" in r]
    rows = sorted((r for r in rows if "error" not in r), key=lambda r: -r["hole_area"])
    for r in rows:
        print("%s tex %s %5d v  hole %10.1f width %7.2f  uncovered %.4f  off %.4f  worst %8.2f  size %8.1f  spacing %.2f" % (
            r["key"], r["tex"], r["verts"], r["hole_area"], r["hole_width"], r["uncovered"], r["off"], r["worst"], r["size"],
            r["spacing"]))
    for r in errs:
        print("%s error %s" % (r["key"], r["error"]))
    if a.csv:
        with open(a.csv, "w") as f:
            f.write("key,tex,verts,uncovered,off,worst,size,spacing\n")
            for r in rows:
                f.write("%s,%s,%d,%.5f,%.5f,%.3f,%.1f,%.3f\n" % (r["key"], r["tex"], r["verts"], r["uncovered"], r["off"],
                                                              r["worst"], r["size"], r["spacing"]))


if __name__ == "__main__":
    main()
