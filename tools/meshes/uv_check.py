"""The remastered meshes' texture coordinates against the game's own (issue: the medkit's lid, its ring wavy, its cross
askew): each corner of a remastered triangle, its place on the original triangle it was made from (tri_source), the
original's coordinates there; how far the remastered ones are, in the texture's texels. With --fix, a vertex whose
triangles all came from originals that agree on its coordinates (not across a seam) gets the original's.

    python tools/meshes/uv_check.py [--dump work/meshes/all] [--pack work/remaster/pack/meshes] [--fix] [--min 1.5]
        [--only key,key] [--csv work/meshes/uv_check.csv]
"""
import argparse
import concurrent.futures
import glob
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fhm  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
K_NEAR = 12  # the original triangles looked at about a vertex


def closest_bary(p, a, b, c):
    """barycentric coordinates of the closest point to p on each triangle (a, b, c), [n, 3]"""
    ab, ac, ap = b - a, c - a, p - a
    d1, d2 = (ab * ap).sum(1), (ac * ap).sum(1)
    bp = p - b
    d3, d4 = (ab * bp).sum(1), (ac * bp).sum(1)
    cp = p - c
    d5, d6 = (ab * cp).sum(1), (ac * cp).sum(1)
    vc = d1 * d4 - d3 * d2
    vb = d5 * d2 - d1 * d6
    va = d3 * d6 - d5 * d4
    out = np.zeros((len(p), 3))
    # inside: the general case, then the regions outside it
    den = va + vb + vc
    den = np.where(np.abs(den) < 1e-20, 1e-20, den)
    v, w = vb / den, vc / den
    out[:] = np.stack([1 - v - w, v, w], 1)
    def setw(mask, bary):
        out[mask] = bary[mask]
    one = np.ones(len(p)); zero = np.zeros(len(p))
    setw((d1 <= 0) & (d2 <= 0), np.stack([one, zero, zero], 1))
    setw((d3 >= 0) & (d4 <= d3), np.stack([zero, one, zero], 1))
    setw((d6 >= 0) & (d5 <= d6), np.stack([zero, zero, one], 1))
    t = np.clip(d1 / np.where(np.abs(d1 - d3) < 1e-20, 1e-20, d1 - d3), 0, 1)
    setw((vc <= 0) & (d1 >= 0) & (d3 <= 0), np.stack([1 - t, t, zero], 1))
    t = np.clip(d2 / np.where(np.abs(d2 - d6) < 1e-20, 1e-20, d2 - d6), 0, 1)
    setw((vb <= 0) & (d2 >= 0) & (d6 <= 0), np.stack([1 - t, zero, t], 1))
    t = np.clip((d4 - d3) / np.where(np.abs((d4 - d3) + (d5 - d6)) < 1e-20, 1e-20, (d4 - d3) + (d5 - d6)), 0, 1)
    setw((va <= 0) & ((d4 - d3) >= 0) & ((d5 - d6) >= 0), np.stack([zero, 1 - t, t], 1))
    return out


def check(path, dump, sizes, fix, tol):
    """per remastered triangle: how its texture coordinates change along its edges against how its original triangle's
    (tri_source) change over the same edges, in texels (its texture stretched, sheared or waved there; where it lies
    or how large the remodel made it does not count). --fix: the coordinates solved again (least squares) to follow
    the originals' over every edge, held lightly where they are"""
    import scipy.sparse as sp
    from scipy.sparse.linalg import lsqr
    key = os.path.basename(path)[:-4]
    orig_path = os.path.join(dump, key + ".fhm")
    if not os.path.exists(orig_path):
        return None
    m, o = fhm.read(path), fhm.read(orig_path)
    # the texture coordinates: register 9, or 5 in the skinned models' layout (the first-person weapons and arms)
    uvr = next((r for r in (9, 5) if m.regs >> r & 1 and o.regs >> r & 1), None)
    if m.tri_source is None or uvr is None:
        return None
    size = sizes.get("%016x" % m.tex, (64, 64))
    tex = np.array(size, float)
    P, UV = m.reg(0)[:, :3].astype(float), m.reg(uvr)[:, :2].astype(float)
    OP, OUV = o.reg(0)[:, :3].astype(float), o.reg(uvr)[:, :2].astype(float)
    t = m.indices.reshape(-1, 3).astype(np.int64)
    ot = o.indices.reshape(-1, 3)
    src = m.tri_source.astype(np.int64)
    keep = src < len(ot)
    t, src = t[keep], src[keep]
    # the original triangle's map: d(uv) = F d(p), F 2x3 (its plane's)
    oa, ob, oc = OP[ot[src, 0]], OP[ot[src, 1]], OP[ot[src, 2]]
    E = np.stack([ob - oa, oc - oa], 2)  # [n, 3, 2]
    D = np.stack([OUV[ot[src, 1]] - OUV[ot[src, 0]], OUV[ot[src, 2]] - OUV[ot[src, 0]]], 2)  # [n, 2, 2]
    G = np.einsum("nji,njk->nik", E, E)  # E^T E
    det = G[:, 0, 0] * G[:, 1, 1] - G[:, 0, 1] * G[:, 1, 0]
    duv_det = D[:, 0, 0] * D[:, 1, 1] - D[:, 0, 1] * D[:, 1, 0]
    # (a triangle and its texture both with area, and not a sliver: its map from so thin a shape is no guide)
    sv = np.linalg.svd(E, compute_uv=False)  # [n, 2]
    good = (np.abs(det) > 1e-12) & (np.abs(duv_det) * tex.prod() > 1e-3) & (sv[:, 1] > 0.08 * sv[:, 0])
    Ginv = np.zeros_like(G)
    Ginv[good, 0, 0], Ginv[good, 1, 1] = G[good, 1, 1] / det[good], G[good, 0, 0] / det[good]
    Ginv[good, 0, 1], Ginv[good, 1, 0] = -G[good, 0, 1] / det[good], -G[good, 1, 0] / det[good]
    F = np.einsum("nij,njk,nlk->nil", D, Ginv, E)  # D (E^T E)^-1 E^T: [n, 2, 3]
    edges = [(0, 1), (1, 2), (2, 0)]
    errs = np.zeros(len(t))
    for i, j in edges:
        dp = P[t[:, j]] - P[t[:, i]]
        pred = np.einsum("nij,nj->ni", F, dp)
        act = UV[t[:, j]] - UV[t[:, i]]
        ab = (np.abs(act - pred) * tex).max(1)
        rel = ab / np.maximum(np.maximum(np.abs(pred), np.abs(act)).max(1) * tex.max(), 1.0)
        errs = np.maximum(errs, np.where(rel > 0.25, ab, 0))  # (a few percent of a long edge: the rounding's own)
    errs = np.where(good, errs, 0)
    # how much of the surface it is (by area): a sliver or a fold's few triangles do not show, a lid's wave does
    area = 0.5 * np.linalg.norm(np.cross(P[t[:, 1]] - P[t[:, 0]], P[t[:, 2]] - P[t[:, 0]]), axis=1)
    share = float(area[errs > tol].sum() / max(area.sum(), 1e-9))
    res = dict(key=key, tex="%016x" % m.tex, size=size, corners=len(t), share=share, worst=float(errs.max()) if len(t) else 0,
               p99=float(np.percentile(errs, 99)) if len(t) else 0, bad=int((errs > tol).sum()), fixed=0)
    if fix and res["bad"]:
        nv = len(P)
        rows, cols, vals, rhs = [], [], [], []
        r = 0
        tg = t[good]
        Fg = F[good]
        for i, j in edges:
            dp = P[tg[:, j]] - P[tg[:, i]]
            pred = np.einsum("nij,nj->ni", Fg, dp)
            n = len(tg)
            idx = np.arange(n) + r
            rows += [idx, idx]; cols += [tg[:, j], tg[:, i]]; vals += [np.ones(n), -np.ones(n)]
            rhs.append(pred)
            r += n
        lam = 0.05
        used = np.unique(tg)
        idx = np.arange(len(used)) + r
        rows.append(idx); cols.append(used); vals.append(np.full(len(used), lam))
        rhs.append(lam * UV[used])
        r += len(used)
        A = sp.csr_matrix((np.concatenate(vals), (np.concatenate(rows), np.concatenate(cols))), shape=(r, nv))
        B = np.concatenate(rhs)
        new = UV.copy()
        for k in range(2):
            new[:, k] = lsqr(A, B[:, k], atol=1e-10, btol=1e-10, iter_lim=20000, x0=UV[:, k])[0]
        # only where the mesh was waved: the vertices of its distorted triangles and those about them
        bad_v = np.unique(t[errs > tol])
        moved = np.zeros(nv, bool); moved[bad_v] = True
        for _ in range(2):
            near = np.zeros(nv, bool)
            sel = moved[t].any(1)
            near[t[sel].ravel()] = True
            moved |= near
        trial = UV.copy()
        trial[moved] = new[moved]
        after = np.zeros(len(t))
        for i, j in edges:
            dp = P[t[:, j]] - P[t[:, i]]
            pred = np.einsum("nij,nj->ni", F, dp)
            act = trial[t[:, j]] - trial[t[:, i]]
            ab = (np.abs(act - pred) * tex).max(1)
            rel = ab / np.maximum(np.maximum(np.abs(pred), np.abs(act)).max(1) * tex.max(), 1.0)
            after = np.maximum(after, np.where(rel > 0.25, ab, 0))
        after = np.where(good, after, 0)
        # kept only if it is better: fewer distorted triangles and none worse than before's worst; the coordinates moved
        # by no more than a tenth of the texture (a whole island would have slid)
        shift = np.abs(trial - UV).max() if moved.any() else 0
        if (after > tol).sum() < res["bad"] and after.max() <= res["worst"] + 0.5 and shift < 0.1:
            i9 = m.reg_list().index(uvr)
            m.vertices[moved, i9, :2] = new[moved].astype(np.float32)
            fhm.write(path, m)
            res["fixed"] = int(moved.sum())
            res["after_bad"] = int((after > tol).sum())
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", default=os.path.join(ROOT, "work", "meshes", "all"))
    ap.add_argument("--pack", default=os.path.join(ROOT, "work", "remaster", "pack", "meshes"))
    ap.add_argument("--fix", action="store_true")
    ap.add_argument("--min", type=float, default=1.5, help="texels off that count as distorted")
    ap.add_argument("--only", default="")
    ap.add_argument("--csv", default=os.path.join(ROOT, "work", "meshes", "uv_check.csv"))
    a = ap.parse_args()
    index = json.load(open(os.path.join(ROOT, "work", "remaster", "extract", "index.json")))
    sizes = {k: (v.get("w", 64), v.get("h", 64)) for k, v in index.items() if isinstance(v, dict)}
    files = sorted(glob.glob(os.path.join(a.pack, "*.fhm")))
    if a.only:
        files = [f for f in files if os.path.basename(f)[:-4] in a.only.split(",")]
    out = []
    with concurrent.futures.ProcessPoolExecutor() as ex:
        for r in ex.map(check, files, [a.dump] * len(files), [sizes] * len(files), [a.fix] * len(files), [a.min] * len(files), chunksize=8):
            if r:
                out.append(r)
    out.sort(key=lambda r: -r["worst"])
    with open(a.csv, "w") as f:
        f.write("key,tex,w,h,corners,share,worst,p99,bad,fixed\n")
        for r in out:
            f.write("%s,%s,%d,%d,%d,%.4f,%.2f,%.2f,%d,%d\n" % (r["key"], r["tex"], r["size"][0], r["size"][1], r["corners"], r["share"],
                                                            r["worst"], r["p99"], r["bad"], r["fixed"]))
    bad = [r for r in out if r["bad"]]
    print("%d meshes checked, %d with corners over %.1f texels, %d vertices fixed" % (len(out), len(bad), a.min, sum(r["fixed"] for r in out)))
    for r in out[:25]:
        print(r["key"], r["tex"], "worst %.1f p99 %.1f bad %d fixed %d" % (r["worst"], r["p99"], r["bad"], r["fixed"]))


if __name__ == "__main__":
    main()
