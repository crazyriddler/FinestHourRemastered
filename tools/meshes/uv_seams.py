"""The remastered meshes' triangles across a seam of the original's texture, cut finer. The original's texture is cut into
islands (a thumb's nail, its side, its back); the remastered triangles do not follow those cuts, so each takes one island
or the other whole and the seam runs in steps across the surface (the first-person thumb). A triangle whose corners lie
over triangles of the original not about the one under its middle without crossing a seam is split in four, twice (16),
each small triangle taking the original's surface about the triangle under its middle for its corners' coordinates (uv_transfer.py's way: the closest point, its own triangle's map carried
on at most 4 texels): the steps 4 times smaller. The new vertices are the triangle's corners blended (position, normal,
the bones' weights: the 3 strongest kept).

    python tools/meshes/uv_seams.py <key>[,<key>...] [--levels 2] [--dump work/meshes/all] [--pack work/remaster/pack/meshes]
"""
import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fhm  # noqa: E402
from uv_transfer import closest_on_triangles, plane_map  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def islands(ot, nverts):
    """each original triangle's island: connected by shared vertices (a seam splits the vertices)"""
    parent = list(range(len(ot)))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    first = [-1] * nverts
    for i, tri in enumerate(ot):
        for v in tri:
            if first[v] < 0:
                first[v] = i
            else:
                a, b = find(i), find(first[v])
                if a != b:
                    parent[a] = b
    return np.array([find(i) for i in range(len(ot))])


def closest_tri(p, A, B, C, chunk=256):
    """the index of the original triangle closest to each point, and its barycentric coordinates"""
    idx = np.zeros(len(p), np.int64)
    bar = np.zeros((len(p), 3))
    for s in range(0, len(p), chunk):
        q = p[s:s + chunk][:, None, :]
        b, d = closest_on_triangles(q, A[None], B[None], C[None])
        j = d.argmin(1)
        idx[s:s + chunk] = j
        bar[s:s + chunk] = b[np.arange(len(j)), j]
    return idx, bar


def corner_uv(p, cand, OP, OUV, ot, size):
    """p's coordinates on the original triangles cand: the closest point on them, carried on by that triangle's map
    (4 texels)"""
    cand = np.array(cand)
    A, B, C = OP[ot[cand, 0]], OP[ot[cand, 1]], OP[ot[cand, 2]]
    b, d = closest_on_triangles(p[None, :], A, B, C)
    j = int(np.argmin(d))
    s = cand[j]
    uv = (OUV[ot[s]] * b[j][:, None]).sum(0)
    ext = plane_map(p, OP[ot[s]], OUV[ot[s]])
    if ext is not None:
        off = (ext - uv) * size
        n = np.abs(off).max()
        uv = ext if n <= 4 else uv + off / n * 4 / size
    return uv, s


def blend(vs, w, regs):
    """a vertex blended from vs [k, nregs, 4] by w [k]: position, normal (normalized), coordinates; the bones' weights by
    bone (the 3 strongest)"""
    out = (vs * w[:, None, None]).sum(0)
    rl = regs
    if 3 in rl:
        n = out[rl.index(3), :3]
        out[rl.index(3), :3] = n / max(np.linalg.norm(n), 1e-9)
    if 1 in rl and 2 in rl:
        acc = {}
        for k in range(len(vs)):
            for c in range(3):
                wt = vs[k, rl.index(1), c] * w[k]
                if wt > 0:
                    bone = vs[k, rl.index(2), c]
                    acc[bone] = acc.get(bone, 0) + wt
        top = sorted(acc.items(), key=lambda kv: -kv[1])[:3]
        tot = sum(x for _, x in top) or 1
        out[rl.index(1), :3] = 0
        out[rl.index(2), :3] = 0
        for c, (bone, wt) in enumerate(top):
            out[rl.index(1), c] = wt / tot
            out[rl.index(2), c] = bone
    return out


def refine(key, dump, pack, sizes, levels):
    path = os.path.join(pack, key + ".fhm")
    m, o = fhm.read(path), fhm.read(os.path.join(dump, key + ".fhm"))
    uvr = next(r for r in (9, 5) if m.regs >> r & 1 and o.regs >> r & 1)
    rl = m.reg_list()
    ir = rl.index(uvr)
    P, OP = m.reg(0)[:, :3].astype(float), o.reg(0)[:, :3].astype(float)
    OUV = o.reg(uvr)[:, :2].astype(float)
    t, ot = m.indices.astype(np.int64), o.indices.astype(np.int64)
    size = np.array(sizes.get("%016x" % m.tex, (256, 256)), float).max()
    # the original's triangles about each one without crossing a seam: those sharing its vertices (a seam splits
    # them), two rings
    by_vertex = [[] for _ in range(len(OP))]
    for i, tri in enumerate(ot):
        for v in tri:
            by_vertex[v].append(i)
    ring1 = [{j for v in tri for j in by_vertex[v]} for tri in ot]
    near = [sorted({k for j in r for k in ring1[j]}) for r in ring1]
    near_set = [set(n) for n in near]
    A, B, C = OP[ot[:, 0]], OP[ot[:, 1]], OP[ot[:, 2]]
    # the original triangle under each vertex and under each triangle's middle: across a seam when a corner's is not
    # about the middle's without crossing one
    vi, _ = closest_tri(P, A, B, C)
    ci, _ = closest_tri(P[t].mean(1), A, B, C)
    straddle = np.array([any(vi[v] not in near_set[ci[i]] for v in t[i]) for i in range(len(t))])
    verts = list(m.vertices)
    tri_src = list(m.tri_source) if m.tri_source is not None else None
    source = list(m.source) if m.source is not None else None
    keep = [i for i in range(len(t)) if not straddle[i]]
    new_tris, new_src = [t[i] for i in keep], [tri_src[i] for i in keep] if tri_src is not None else None
    # the sub-triangles in the parent's barycentric coordinates
    subs = [np.eye(3)]
    for _ in range(levels):
        nxt = []
        for s in subs:
            a, b, c = s
            ab, bc, ca = (a + b) / 2, (b + c) / 2, (c + a) / 2
            nxt += [np.array([a, ab, ca]), np.array([ab, b, bc]), np.array([ca, bc, c]), np.array([ab, bc, ca])]
        subs = nxt
    made = 0
    for i in np.nonzero(straddle)[0]:
        pv = m.vertices[t[i]]
        for s in subs:
            mid = s.mean(0)
            pmid = mid @ P[t[i]]
            k, _ = closest_tri(pmid[None, :], A, B, C)
            cand = near[k[0]]
            tri = []
            for c in range(3):
                vtx = blend(pv, s[c], rl)
                uv, src = corner_uv(vtx[0, :3].astype(float), cand, OP, OUV, ot, size)
                vtx[ir, :2] = uv
                tri.append(len(verts))
                verts.append(vtx.astype(np.float32))
                if source is not None:
                    source.append(m.source[t[i][int(np.argmax(s[c]))]])
            new_tris.append(np.array(tri))
            if new_src is not None:
                new_src.append(src)
            made += 1
    m.vertices = np.array(verts, np.float32)
    m.indices = np.array(new_tris, np.uint32)
    if new_src is not None:
        m.tri_source = np.array(new_src, np.uint32)
    if source is not None:
        m.source = np.array(source, np.uint32)
    fhm.write(path, m)
    return int(straddle.sum()), made


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("keys")
    ap.add_argument("--levels", type=int, default=2)
    ap.add_argument("--dump", default=os.path.join(ROOT, "work", "meshes", "all"))
    ap.add_argument("--pack", default=os.path.join(ROOT, "work", "remaster", "pack", "meshes"))
    a = ap.parse_args()
    index = json.load(open(os.path.join(ROOT, "work", "remaster", "extract", "index.json")))
    sizes = {k: (v.get("w", 64), v.get("h", 64)) for k, v in index.items() if isinstance(v, dict)}
    for key in a.keys.split(","):
        n, made = refine(key, a.dump, a.pack, sizes, a.levels)
        print("%s: %d triangles across a seam, cut into %d" % (key, n, made))


if __name__ == "__main__":
    main()
