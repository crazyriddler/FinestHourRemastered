"""A remastered mesh's texture coordinates made again from the original's surface, for the meshes whose coordinates
had drifted too far for uv_check.py --fix (the weapons' stocks in first person: the PPSh's, the G43's, the Mosin's...,
stretched over their wood). Each corner of a remastered triangle takes the coordinates of the closest point on the
original's surface about the triangle it was made from (tri_source and the original triangles around it that share its
texture island, two rings), not extrapolated beyond them. Only the triangles off their original's map (uv_check.py's
measure) are made again, each corner moved getting its own copy of the vertex: a triangle across a seam between two
islands of the texture (the wood smeared over the gap) keeps to its own island; the rest are left as they are.

    python tools/meshes/uv_transfer.py <key>[,<key>...] [--dump work/meshes/all] [--pack work/remaster/pack/meshes]
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fhm  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def closest_on_triangles(p, a, b, c):
    """the closest point to p on each triangle (a, b, c): its barycentric coordinates [n, 3] and squared distance"""
    ab, ac, ap = b - a, c - a, p - a
    d1, d2 = (ab * ap).sum(-1), (ac * ap).sum(-1)
    bp = p - b
    d3, d4 = (ab * bp).sum(-1), (ac * bp).sum(-1)
    cp = p - c
    d5, d6 = (ab * cp).sum(-1), (ac * cp).sum(-1)
    va, vb, vc = d3 * d6 - d5 * d4, d5 * d2 - d1 * d6, d1 * d4 - d3 * d2
    den = va + vb + vc
    den = np.where(np.abs(den) < 1e-20, 1e-20, den)
    v, w = vb / den, vc / den
    bary = np.stack([1 - v - w, v, w], -1)
    # outside: the closest point on the nearest edge or corner (clamped, then the best of the three edges)
    out = (bary < 0).any(-1)
    if out.any():
        best = np.full(bary.shape[:-1], np.inf)
        res = bary.copy()
        for (x, y, ix, iy) in ((a, b, 0, 1), (b, c, 1, 2), (a, c, 0, 2)):
            e = y - x
            t = np.clip(((p - x) * e).sum(-1) / np.maximum((e * e).sum(-1), 1e-20), 0, 1)
            q = x + e * t[..., None]
            dd = ((p - q) ** 2).sum(-1)
            take = out & (dd < best)
            best = np.where(take, dd, best)
            r = np.zeros_like(bary)
            r[..., ix], r[..., iy] = 1 - t, t
            res = np.where(take[..., None], r, res)
        bary = res
    q = a * bary[..., 0:1] + b * bary[..., 1:2] + c * bary[..., 2:3]
    return bary, ((p - q) ** 2).sum(-1)


def plane_map(p, tri, uvs):
    """the texture coordinates of p by the triangle's map extended over its plane (p projected on it), None for a
    sliver"""
    a, b, c = tri
    e1, e2 = b - a, c - a
    g = np.array([[e1 @ e1, e1 @ e2], [e1 @ e2, e2 @ e2]])
    if abs(np.linalg.det(g)) < 1e-12:
        return None
    v, w = np.linalg.solve(g, np.array([(p - a) @ e1, (p - a) @ e2]))
    return uvs[0] + (uvs[1] - uvs[0]) * v + (uvs[2] - uvs[0]) * w


def bad_triangles(P, UV, OP, OUV, t, ot, src, tex, tol=1.5):
    """uv_check.py's measure: a triangle whose coordinates are off its original's map by more than tol texels"""
    oa, ob, oc = OP[ot[src, 0]], OP[ot[src, 1]], OP[ot[src, 2]]
    E = np.stack([ob - oa, oc - oa], 2)
    D = np.stack([OUV[ot[src, 1]] - OUV[ot[src, 0]], OUV[ot[src, 2]] - OUV[ot[src, 0]]], 2)
    G = np.einsum("nji,njk->nik", E, E)
    det = G[:, 0, 0] * G[:, 1, 1] - G[:, 0, 1] * G[:, 1, 0]
    duv_det = D[:, 0, 0] * D[:, 1, 1] - D[:, 0, 1] * D[:, 1, 0]
    sv = np.linalg.svd(E, compute_uv=False)
    good = (np.abs(det) > 1e-12) & (np.abs(duv_det) * tex.prod() > 1e-3) & (sv[:, 1] > 0.08 * sv[:, 0])
    Ginv = np.zeros_like(G)
    Ginv[good, 0, 0], Ginv[good, 1, 1] = G[good, 1, 1] / det[good], G[good, 0, 0] / det[good]
    Ginv[good, 0, 1], Ginv[good, 1, 0] = -G[good, 0, 1] / det[good], -G[good, 1, 0] / det[good]
    F = np.einsum("nij,njk,nlk->nil", D, Ginv, E)
    errs = np.zeros(len(t))
    for i, j in ((0, 1), (1, 2), (2, 0)):
        dp = P[t[:, j]] - P[t[:, i]]
        pred = np.einsum("nij,nj->ni", F, dp)
        act = UV[t[:, j]] - UV[t[:, i]]
        ab = (np.abs(act - pred) * tex).max(1)
        rel = ab / np.maximum(np.maximum(np.abs(pred), np.abs(act)).max(1) * tex.max(), 1.0)
        errs = np.maximum(errs, np.where(rel > 0.25, ab, 0))
    return good & (errs > tol)


def transfer(key, dump, pack, sizes):
    path = os.path.join(pack, key + ".fhm")
    m, o = fhm.read(path), fhm.read(os.path.join(dump, key + ".fhm"))
    uvr = next(r for r in (9, 5) if m.regs >> r & 1 and o.regs >> r & 1)
    P, OP = m.reg(0)[:, :3].astype(float), o.reg(0)[:, :3].astype(float)
    OUV = o.reg(uvr)[:, :2].astype(float)
    t, ot = m.indices.astype(np.int64), o.indices.astype(np.int64)
    src = m.tri_source.astype(np.int64)
    valid = src < len(ot)  # (a triangle with no original's: left as it is)
    src = np.where(valid, src, 0)
    # the original's triangles about each one in its island: those sharing a vertex (a seam splits the vertices)
    by_vertex = [[] for _ in range(len(OP))]
    for i, tri in enumerate(ot):
        for v in tri:
            by_vertex[v].append(i)
    ring1 = [sorted({j for v in tri for j in by_vertex[v]}) for tri in ot]
    near = [sorted({k for j in r for k in ring1[j]}) for r in ring1]
    UV = m.reg(uvr)[:, :2].astype(float)
    size = np.array(sizes.get("%016x" % m.tex, (256, 256)), float)
    bad = bad_triangles(P, UV, OP, OUV, t, ot, src, size) & valid
    ir = m.reg_list().index(uvr)
    verts = list(m.vertices)
    source = list(m.source) if m.source is not None else None
    redone = 0
    for i in np.nonzero(bad)[0]:
        s = src[i]
        cand = np.array(near[s])
        A, B, C = OP[ot[cand, 0]], OP[ot[cand, 1]], OP[ot[cand, 2]]
        for k in range(3):
            v = int(t[i, k])
            bary, d = closest_on_triangles(P[v][None, :], A, B, C)
            j = int(np.argmin(d))
            uv = (OUV[ot[cand[j]]] * bary[j][:, None]).sum(0)
            # over a rounded edge the closest point lies on the original's edge: the texture drawn out across the
            # bevel. Its own original triangle's map carried on past the edge instead (the face's texture going on),
            # at most 4 texels beyond the closest point (no further into what lies beside its island)
            ext = plane_map(P[v], OP[ot[s]], OUV[ot[s]])
            if ext is not None:
                # (from the closest point on its own triangle: one beside it may lie in another island)
                b0, _ = closest_on_triangles(P[v][None, :], OP[ot[s, 0]][None], OP[ot[s, 1]][None], OP[ot[s, 2]][None])
                uv = (OUV[ot[s]] * b0[0][:, None]).sum(0)
                off = (ext - uv) * size
                n = np.abs(off).max()
                uv = ext if n <= 4 else uv + off / n * 4 / size
            if np.abs(uv - UV[v]).max() * size.max() < 0.5:
                continue
            nv = m.vertices[v].copy()
            nv[ir, :2] = uv
            t[i, k] = len(verts)
            verts.append(nv)
            if source is not None:
                source.append(m.source[v])
        redone += 1
    n0 = len(P)
    m.vertices = np.array(verts, np.float32)
    m.indices = t.astype(np.uint32)
    if source is not None:
        m.source = np.array(source, np.uint32)
    fhm.write(path, m)
    return int(bad.sum()), redone, len(verts) - n0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("keys")
    ap.add_argument("--dump", default=os.path.join(ROOT, "work", "meshes", "all"))
    ap.add_argument("--pack", default=os.path.join(ROOT, "work", "remaster", "pack", "meshes"))
    a = ap.parse_args()
    import json
    index = json.load(open(os.path.join(ROOT, "work", "remaster", "extract", "index.json")))
    sizes = {k: (v.get("w", 64), v.get("h", 64)) for k, v in index.items() if isinstance(v, dict)}
    for key in a.keys.split(","):
        nbad, redone, added = transfer(key, a.dump, a.pack, sizes)
        print("%s: %d triangles off their original's map, %d redone, %d vertices added" % (key, nbad, redone, added))


if __name__ == "__main__":
    main()
