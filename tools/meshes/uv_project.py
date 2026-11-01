"""A remastered mesh's texture coordinates made again by projecting it onto the original: each vertex along its normal
(both ways, the nearer hit) onto the original's surface, the coordinates there. Unlike the closest point (which put a
rounded edge's whole bevel onto the original's edge: the texture drawn out across it) the normals spread a bevel over the
two faces it rounds, and a vertex keeps one value for all its triangles (no steps between neighbours: the first-person
thumb's patches). A triangle whose corners land on the original about different places (not reachable without crossing
a seam of its texture) is cut into 4 (--levels 1; 16 with 2) (uv_seams.py's blend of the corners), each small triangle projected onto the
original's surface about the place under its middle alone; its neighbours are cut at the same points along the edges
they share (the same vertex data both sides: no crack where the bones bend it), fanned about their middles, their new
points projected about the place under their middles alone (one landing in the island beside drew a thin stripe of
what lies between the islands: a dashed line along the thumb).

    python tools/meshes/uv_project.py <key>[,<key>...] [--from work/meshes/backup_reg5] [--levels 2]
"""
import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fhm  # noqa: E402
from uv_seams import blend, closest_tri  # noqa: E402
from uv_transfer import closest_on_triangles, plane_map  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def ray_hits(o, d, A, B, C, reach, chunk=128):
    """each ray (o, d, both ways) against the triangles: the nearest hit within reach (index, barycentric), -1 none"""
    e1, e2 = B - A, C - A
    idx = np.full(len(o), -1)
    bar = np.zeros((len(o), 3))
    for s in range(0, len(o), chunk):
        oo, dd = o[s:s + chunk, None, :], d[s:s + chunk, None, :]
        p = np.cross(dd, e2[None])
        det = (e1[None] * p).sum(-1)
        ok = np.abs(det) > 1e-12
        inv = np.where(ok, 1 / np.where(ok, det, 1), 0)
        tv = oo - A[None]
        u = (tv * p).sum(-1) * inv
        q = np.cross(tv, e1[None])
        v = (dd * q).sum(-1) * inv
        t = (e2[None] * q).sum(-1) * inv
        eps = 1e-4
        hit = ok & (u >= -eps) & (v >= -eps) & (u + v <= 1 + eps) & (np.abs(t) <= reach)
        at = np.where(hit, np.abs(t), np.inf)
        j = at.argmin(1)
        good = np.isfinite(at[np.arange(len(j)), j])
        r = np.arange(len(j))
        uu, vv = u[r, j], v[r, j]
        idx[s:s + chunk] = np.where(good, j, -1)
        bar[s:s + chunk] = np.stack([1 - uu - vv, uu, vv], -1)
    return idx, np.clip(bar, 0, 1) / np.clip(bar, 0, 1).sum(1, keepdims=True)


def project(points, normals, A, B, C, OUV, ot, reach, cand=None):
    """the coordinates for points: their normals' hit on the original (restricted to cand), else the closest point"""
    sel = np.arange(len(ot)) if cand is None else np.asarray(cand)
    idx, bar = ray_hits(points, normals, A[sel], B[sel], C[sel], reach)
    miss = idx < 0
    if miss.any():
        ci, cb = closest_tri(points[miss], A[sel], B[sel], C[sel])
        idx[miss], bar[miss] = ci, cb
    tri = sel[idx]
    return (OUV[ot[tri]] * bar[:, :, None]).sum(1), tri


def run(key, src_dir, dump, pack, levels):
    m = fhm.read(os.path.join(src_dir, key + ".fhm"))
    o = fhm.read(os.path.join(dump, key + ".fhm"))
    uvr = next(r for r in (9, 5) if m.regs >> r & 1 and o.regs >> r & 1)
    rl = m.reg_list()
    ir = rl.index(uvr)
    P = m.reg(0)[:, :3].astype(float)
    N = m.reg(3)[:, :3].astype(float) if 3 in rl else None
    OP, OUV = o.reg(0)[:, :3].astype(float), o.reg(uvr)[:, :2].astype(float)
    t, ot = m.indices.astype(np.int64), o.indices.astype(np.int64)
    A, B, C = OP[ot[:, 0]], OP[ot[:, 1]], OP[ot[:, 2]]
    reach = 0.08 * float(np.ptp(OP, axis=0).max())
    if N is None:
        return None
    by_vertex = [[] for _ in range(len(OP))]
    for i, tri in enumerate(ot):
        for v in tri:
            by_vertex[v].append(i)
    ring1 = [{j for v in tri for j in by_vertex[v]} for tri in ot]
    near = [sorted({k for j in r for k in ring1[j]}) for r in ring1]
    near_set = [set(n) for n in near]

    def island_uv(pts, nrm, k):
        """points' coordinates in the island of the original triangle k: projected onto the triangles sharing its
        vertices (one ring: a seam ending further on can not join the island across it), else the closest point on
        them"""
        ring = sorted(ring1[k])
        idx, bar = ray_hits(pts, nrm, A[ring], B[ring], C[ring], reach)
        out = np.zeros((len(pts), 2))
        for q in range(len(pts)):
            if idx[q] >= 0:
                out[q] = (OUV[ot[ring[idx[q]]]] * bar[q][:, None]).sum(0)
            else:
                # (missed: the closest point on them, kept inside the island; carried on past its edge it took the
                # dark between the islands, teeth along the seam)
                b2, d2 = closest_on_triangles(pts[q][None, :], A[ring], B[ring], C[ring])
                j = int(np.argmin(d2))
                out[q] = (OUV[ot[ring[j]]] * b2[j][:, None]).sum(0)
        return out
    uv, vtri = project(P, N, A, B, C, OUV, ot, reach)
    mid = P[t].mean(1)
    mn = N[t].sum(1)
    mn /= np.maximum(np.linalg.norm(mn, axis=1, keepdims=True), 1e-9)
    _, mtri = project(mid, mn, A, B, C, OUV, ot, reach)
    # (one ring: two rings could reach round the end of a seam into the island across it)
    straddle = np.array([any(vtri[v] not in ring1[mtri[i]] for v in t[i]) for i in range(len(t))])
    verts = m.vertices.copy()
    verts[:, ir, :2] = uv
    verts = list(verts)
    source = list(m.source) if m.source is not None else None
    tri_src = m.tri_source
    n_split = 1 << levels
    # the points on a cut triangle's edges made once, by the edge's two ends (by their positions: a vertex split by
    # the remaster's own seams is the same place), the same data on both sides of the edge: a neighbour cut along it
    # too, the bones moving its points as they move the cut triangle's (no crack at a T-junction: the bent thumb)
    pk = [tuple(np.round(p, 4)) for p in P]
    edge_cache = {}

    def edge_point(va, vb, f):
        if pk[va] > pk[vb]:
            va, vb, f = vb, va, 1 - f
        key = (pk[va], pk[vb], round(f * n_split))
        if key not in edge_cache:
            edge_cache[key] = blend(m.vertices[[va, vb]], np.array([1 - f, f]), rl)
        return edge_cache[key].copy()

    def corner_data(tri, b):
        nz = np.nonzero(b > 1e-9)[0]
        if len(nz) == 1:
            return m.vertices[tri[nz[0]]].copy()
        if len(nz) == 2:
            a, c = nz
            return edge_point(tri[a], tri[c], b[c] / (b[a] + b[c]))
        return blend(m.vertices[tri], b, rl)

    def add_vertex(data, src_v):
        verts.append(data.astype(np.float32))
        if source is not None:
            source.append(m.source[src_v])
        return len(verts) - 1

    # each triangle keeps the original's triangle its parent was made from (tri_source): the runtime shows or hides
    # the weapon in hand's pieces by it (WeaponPieces), and the one under a small triangle's middle was at times a hidden
    # piece: holes along the seams (the first-person thumb's dashed line)
    def parent_src(i):
        return tri_src[i] if tri_src is not None else mtri[i]

    cut_edges = set()
    for i in np.nonzero(straddle)[0]:
        for a, b in ((0, 1), (1, 2), (2, 0)):
            cut_edges.add(frozenset((pk[t[i, a]], pk[t[i, b]])))
    new_tris, new_src = [], []
    subs = [np.eye(3)]
    for _ in range(levels):
        nxt = []
        for sb in subs:
            a, b, c = sb
            ab, bc, ca = (a + b) / 2, (b + c) / 2, (c + a) / 2
            nxt += [np.array([a, ab, ca]), np.array([ab, b, bc]), np.array([ca, bc, c]), np.array([ab, bc, ca])]
        subs = nxt
    for i in range(len(t)):
        tri = t[i]
        if straddle[i]:
            for sb in subs:
                vs = np.array([corner_data(tri, sb[c]) for c in range(3)])
                pm = vs[:, 0, :3].mean(0)
                nm = vs[:, rl.index(3), :3].sum(0)
                nm /= max(np.linalg.norm(nm), 1e-9)
                _, k = project(pm[None].astype(float), nm[None].astype(float), A, B, C, OUV, ot, reach)
                cuv = island_uv(vs[:, 0, :3].astype(float), vs[:, rl.index(3), :3].astype(float), k[0])
                out = []
                for c in range(3):
                    vs[c, ir, :2] = cuv[c]
                    out.append(add_vertex(vs[c], tri[int(np.argmax(sb[c]))]))
                new_tris.append(np.array(out))
                new_src.append(parent_src(i))
            continue
        split = [frozenset((pk[tri[a]], pk[tri[b]])) in cut_edges for a, b in ((0, 1), (1, 2), (2, 0))]
        if not any(split):
            new_tris.append(tri)
            new_src.append(parent_src(i))
            continue
        # a neighbour of a cut triangle: its edges' points as the cut one has them, a fan about its middle
        ring = []
        cuv = island_uv(P[tri], N[tri], mtri[i])
        for e, (a, b) in enumerate(((0, 1), (1, 2), (2, 0))):
            d = m.vertices[tri[a]].copy()
            d[ir, :2] = cuv[a]
            ring.append(add_vertex(d, tri[a]))
            if split[e]:
                for q in range(1, n_split):
                    d = edge_point(tri[a], tri[b], q / n_split)
                    d[ir, :2] = island_uv(d[0, :3][None].astype(float), d[rl.index(3), :3][None].astype(float), mtri[i])[0]
                    ring.append(add_vertex(d, tri[a]))
        cd = blend(m.vertices[tri], np.full(3, 1 / 3), rl)
        cd[ir, :2] = island_uv(cd[0, :3][None].astype(float), cd[rl.index(3), :3][None].astype(float), mtri[i])[0]
        centre = add_vertex(cd, tri[0])
        for q in range(len(ring)):
            new_tris.append(np.array([centre, ring[q], ring[(q + 1) % len(ring)]]))
            new_src.append(parent_src(i))
    # the same vertex made more than once (the small triangles' corners, each its own) kept once
    vv = np.array(verts, np.float32)
    keyed = vv.reshape(len(vv), -1)
    if source is not None:
        keyed = np.concatenate([keyed, np.array(source, np.float32)[:, None]], 1)
    _, first, remap = np.unique(np.ascontiguousarray(keyed).view(np.dtype((np.void, keyed.dtype.itemsize * keyed.shape[1]))).ravel(),
                                return_index=True, return_inverse=True)
    order = np.argsort(first)
    rank = np.empty_like(order)
    rank[order] = np.arange(len(order))
    m.vertices = vv[first[order]]
    m.indices = rank[remap.ravel()][np.array(new_tris, np.int64)].astype(np.uint32)
    if source is not None:
        source = list(np.array(source)[first[order]])
    if tri_src is not None:
        m.tri_source = np.array(new_src, np.uint32)
    if source is not None:
        m.source = np.array(source, np.uint32)
    fhm.write(os.path.join(pack, key + ".fhm"), m)
    return int(straddle.sum()), len(t)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("keys")
    ap.add_argument("--from", dest="src", default=os.path.join(ROOT, "work", "meshes", "backup_reg5"))
    ap.add_argument("--levels", type=int, default=1)
    ap.add_argument("--dump", default=os.path.join(ROOT, "work", "meshes", "all"))
    ap.add_argument("--pack", default=os.path.join(ROOT, "work", "remaster", "pack", "meshes"))
    a = ap.parse_args()
    for key in a.keys.split(","):
        r = run(key, a.src, a.dump, a.pack, a.levels)
        print("%s: %s" % (key, "no normals" if r is None else "%d of %d triangles across a seam, cut" % r))


if __name__ == "__main__":
    main()
