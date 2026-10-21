"""A game mesh (.fhm dump) made smooth and dense in Blender, keeping everything the game's vertex program or fixed
function pipeline reads from it, whatever its registers hold.

Blender subdivides the shape alone. Each new face remembers the original triangle it came from, and each corner of
the new mesh takes its registers from the original surface where it lies nearest (barycentric in that triangle):
texture coordinates, colours, anything continuous; bone indices from the nearest corner; bone weights summed by bone
over the three corners, the 3 strongest kept. The nearest point is looked for on its own triangle and those about it
(two rings) of the same UV island (no seam crossed) and facing the same way (not a thin part's other side): the
rounding moves a corner off its own triangle where the shape curves, and kept on it the texture was squeezed to the
triangle's edge and smeared along it. So the texture shows on the new shape as it showed on the original. The
normals are the new shape's own. UV seams, prelit colours and skinning come out as the original had them, for a
character, a vehicle or a weapon alike.

The original's smoothing tells where it is meant to be round and where hard: an edge whose two faces share their
normals is smooth and subdivides round; one split in normals (a hard edge) is creased and stays sharp; on hard
surfaces (--crease-angle) folds sharper than the angle are creased too.

    blender -b --factory-startup --python tools/meshes/subdivide.py -- <in.fhm> <out.fhm> [--levels 2]
        [--crease-angle 70] [--blend out.blend]
"""
import argparse
import math
import os
import sys

import bmesh
import bpy
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fhm  # noqa: E402

HARD_NORMAL = 0.985  # normals less alike than this (about 10 degrees) across an edge: a hard edge


def roles(m):
    """what each register holds, from its values: {reg: "position" | "normal" | "indices" | "weights" | "value"}.
    v0 is the position (the programs' and the fixed function's); whole numbers are bone indices, and beside them
    values summing to 1 are their weights (told first: a weight of 1 on one bone, (1, 0, 0), is a unit vector too);
    of the rest, a unit vector everywhere is a normal"""
    out = {}
    regs = m.reg_list()
    for r in regs:
        v = m.reg(r)[:, :3]
        if r == 0:
            out[r] = "position"
        elif len(v) and np.ptp(m.reg(r), axis=0).max() < 1e-6:
            out[r] = "value"  # the same everywhere (a rigid model's one bone and its weight 1): kept as it is
        elif len(v) and np.all(np.abs(v - np.round(v)) < 1e-4) and v.max() > 1.5:
            out[r] = "indices"
    if "indices" in out.values():
        for r in regs:
            if r in out:
                continue
            v = m.reg(r)[:, :3]
            if v.min() > -1e-4 and v.max() < 1 + 1e-4 and np.mean(np.abs(v.sum(1) - 1) < 0.02) > 0.95:
                out[r] = "weights"
                break
    for r in regs:
        if r in out:
            continue
        v = m.reg(r)[:, :3]
        out[r] = "normal" if len(v) and np.mean(np.abs(np.linalg.norm(v, axis=1) - 1) < 0.02) > 0.95 else "value"
    return out


def weld(m):
    """the dump's split vertices joined by position: (positions, per-vertex index of its welded point)"""
    p = m.reg(0)[:, :3]
    key = np.round(p, 4)
    uniq, inverse = np.unique(key, axis=0, return_inverse=True)
    first = np.full(len(uniq), -1)
    for i, w in enumerate(inverse.ravel()):
        if first[w] < 0:
            first[w] = i
    return p[first], inverse.ravel()


def build(m, name, crease_angle=0.0):
    """the mesh welded, each face with its original triangle (face attribute "src"), hard edges creased"""
    pos, wid = weld(m)
    faces, srcs = [], []
    seen = set()
    for ti, t in enumerate(m.indices):
        a, b, c = int(t[0]), int(t[1]), int(t[2])
        wa, wb, wc = wid[a], wid[b], wid[c]
        if wa == wb or wb == wc or wa == wc:
            continue
        # the same face twice (a strip's repeats); facing the other way is another face (double-sided parts)
        r = min((wa, wb, wc), (wb, wc, wa), (wc, wa, wb))
        if r in seen:
            continue
        seen.add(r)
        faces.append((wa, wb, wc))
        srcs.append(ti)
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(pos.tolist(), [], faces)
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    src = mesh.attributes.new("src", "INT", "FACE")
    src.data.foreach_set("value", srcs)

    rl = roles(m)
    normal_reg = next((r for r, k in rl.items() if k == "normal"), None)
    normals = m.reg(normal_reg)[:, :3] if normal_reg is not None else None
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bm.faces.ensure_lookup_table()
    bm.normal_update()
    crease = bm.edges.layers.float.new("crease_edge")
    corner_of = {}  # (face, welded vertex) -> original vertex
    for fi, f in enumerate(bm.faces):
        tri = m.indices[srcs[fi]]
        for k, v in enumerate(f.verts):
            corner_of[(fi, v.index)] = int(tri[k])
    hard = 0
    for e in bm.edges:
        lf = e.link_faces
        if len(lf) != 2:
            e[crease] = 1.0 if len(lf) == 1 else 0.0
            continue
        if crease_angle > 0:
            fold = math.degrees(lf[0].normal.angle(lf[1].normal, 0.0))
            ramp = min(max((fold - (crease_angle - 15)) / 15, 0.0), 1.0)
            if ramp > 0:
                e[crease] = max(e[crease], ramp)
                if ramp >= 1:
                    e.smooth = False
                    hard += 1
        if normals is None:
            continue
        split = False
        for v in e.verts:
            n0 = normals[corner_of[(lf[0].index, v.index)]]
            n1 = normals[corner_of[(lf[1].index, v.index)]]
            if float(np.dot(n0, n1)) < HARD_NORMAL:
                split = True
        if split:
            e[crease] = 1.0
            e.smooth = False
            hard += 1
    bm.to_mesh(mesh)
    bm.free()
    print(f"{name}: {len(pos)} points, {len(faces)} faces, {hard} hard edges, registers {rl}")
    return obj


def apply_subdivision(obj, levels):
    mod = obj.modifiers.new("subd", "SUBSURF")
    mod.levels = levels
    mod.render_levels = levels
    mod.boundary_smooth = "PRESERVE_CORNERS"
    mod.use_creases = True
    mod.use_limit_surface = True
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.modifier_apply(modifier=mod.name)
    tri = obj.modifiers.new("tri", "TRIANGULATE")
    tri.quad_method = "SHORTEST_DIAGONAL"
    bpy.ops.object.modifier_apply(modifier=tri.name)


def closest(p, a, b, c):
    """the point of each triangle (a, b, c) nearest p, as its weights in it: ([n, 3] weights, [n] distances)
    (Ericson, Real-Time Collision Detection 5.1.5, over arrays)"""
    ab, ac, ap = b - a, c - a, p - a
    d1, d2 = (ab * ap).sum(1), (ac * ap).sum(1)
    bp = p - b
    d3, d4 = (ab * bp).sum(1), (ac * bp).sum(1)
    cp = p - c
    d5, d6 = (ab * cp).sum(1), (ac * cp).sum(1)
    va, vb, vc = d3 * d6 - d5 * d4, d5 * d2 - d1 * d6, d1 * d4 - d3 * d2
    n = len(p)
    w = np.zeros((n, 3))
    done = np.zeros(n, bool)

    def put(mask, bw):
        nonlocal done
        m = mask & ~done
        w[m] = bw[m] if bw.ndim == 2 else bw
        done |= m

    eps = 1e-30
    put((d1 <= 0) & (d2 <= 0), np.array([1.0, 0, 0]))
    put((d3 >= 0) & (d5 <= d3), np.array([0, 1.0, 0]))
    t = d1 / np.where(np.abs(d1 - d3) < eps, eps, d1 - d3)
    put((vc <= 0) & (d1 >= 0) & (d3 <= 0), np.stack([1 - t, t, np.zeros(n)], 1))
    put((d6 >= 0) & (d4 <= d6), np.array([0, 0, 1.0]))
    t = d2 / np.where(np.abs(d2 - d6) < eps, eps, d2 - d6)
    put((vb <= 0) & (d2 >= 0) & (d6 <= 0), np.stack([1 - t, np.zeros(n), t], 1))
    den = (d4 - d3) + (d5 - d6)
    t = (d4 - d3) / np.where(np.abs(den) < eps, eps, den)
    put((va <= 0) & ((d4 - d3) >= 0) & ((d5 - d6) >= 0), np.stack([np.zeros(n), 1 - t, t], 1))
    den = va + vb + vc
    den = np.where(np.abs(den) < eps, eps, den)
    put(np.ones(n, bool), np.stack([va / den, vb / den, vc / den], 1))
    w = np.clip(w, 0, 1)  # (a triangle with no area: its weights as they come, kept to the triangle)
    w /= np.maximum(w.sum(1, keepdims=True), 1e-12)
    q = a * w[:, :1] + b * w[:, 1:2] + c * w[:, 2:]
    return w, np.linalg.norm(p - q, axis=1)


def neighbours(m, rl, rings=2):
    """each original triangle's own and those about it (sharing an edge, as far as `rings` steps) of its UV island:
    across the edge both corners alike in every register but the position and normal (the texture coordinates,
    colours, bones)"""
    pos, wid = weld(m)
    vals = [r for r, k in rl.items() if k in ("value", "indices", "weights")]  # (a piece on another bone: another)
    V = np.concatenate([m.reg(r) for r in vals], 1) if vals else np.zeros((len(wid), 0))
    tris = m.indices
    edges = {}
    for ti, t in enumerate(tris):
        for k in range(3):
            u, v = int(t[k]), int(t[(k + 1) % 3])
            key = (min(wid[u], wid[v]), max(wid[u], wid[v]))
            if key[0] != key[1]:
                edges.setdefault(key, []).append((ti, u, v))
    adj = [set() for _ in range(len(tris))]
    for (wu, wv), lst in edges.items():
        for i in range(len(lst)):
            for j in range(i + 1, len(lst)):
                (t0, u0, v0), (t1, u1, v1) = lst[i], lst[j]
                # the same corners of the edge on each side (by welded point)
                a0, b0 = (u0, v0) if wid[u0] == wu else (v0, u0)
                a1, b1 = (u1, v1) if wid[u1] == wu else (v1, u1)
                if np.abs(V[a0] - V[a1]).max(initial=0) < 1e-4 and np.abs(V[b0] - V[b1]).max(initial=0) < 1e-4:
                    adj[t0].add(t1)
                    adj[t1].add(t0)
    out = []
    for ti in range(len(tris)):
        seen, ring = {ti}, {ti}
        for _ in range(rings):
            ring = {n for r in ring for n in adj[r]} - seen
            seen |= ring
        out.append(np.array(sorted(seen)))
    return out


def barycentric(p, a, b, c):
    """p's weights in the triangle (a, b, c), projected on its plane and kept inside it: [n, 3]"""
    v0, v1, v2 = b - a, c - a, p - a
    d00 = (v0 * v0).sum(1)
    d01 = (v0 * v1).sum(1)
    d11 = (v1 * v1).sum(1)
    d20 = (v2 * v0).sum(1)
    d21 = (v2 * v1).sum(1)
    den = d00 * d11 - d01 * d01
    den = np.where(np.abs(den) < 1e-12, 1e-12, den)
    v = (d11 * d20 - d01 * d21) / den
    w = (d00 * d21 - d01 * d20) / den
    bc = np.stack([1 - v - w, v, w], 1)
    bc = np.clip(bc, 0, None)
    return bc / np.maximum(bc.sum(1, keepdims=True), 1e-9)


def export(obj, src):
    mesh = obj.data
    mesh.shade_smooth()
    crease = mesh.attributes.get("crease_edge")
    if crease is not None:
        c = np.zeros(len(mesh.edges), np.float32)
        crease.data.foreach_get("value", c)
        sharp = mesh.attributes.get("sharp_edge") or mesh.attributes.new("sharp_edge", "BOOLEAN", "EDGE")
        sharp.data.foreach_set("value", (c > 0.5).tolist())
    mesh.calc_loop_triangles()
    nt = len(mesh.loop_triangles)
    tri_loops = np.zeros(nt * 3, np.int32)
    mesh.loop_triangles.foreach_get("loops", tri_loops)
    tri_poly = np.zeros(nt, np.int32)
    mesh.loop_triangles.foreach_get("polygon_index", tri_poly)
    nl = len(mesh.loops)
    loop_v = np.zeros(nl, np.int32)
    mesh.loops.foreach_get("vertex_index", loop_v)
    loop_n = np.zeros(nl * 3, np.float32)
    mesh.corner_normals.foreach_get("vector", loop_n)
    loop_n = loop_n.reshape(-1, 3)
    co = np.zeros(len(mesh.vertices) * 3, np.float32)
    mesh.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3)
    poly_src = np.zeros(len(mesh.polygons), np.int32)
    mesh.attributes["src"].data.foreach_get("value", poly_src)

    # each corner of the new triangles: the original surface where it lies nearest, about the triangle it came from
    regs = src.reg_list()
    rl = roles(src)
    corners = tri_loops  # loops, 3 a triangle
    from_tri = poly_src[np.repeat(tri_poly, 3)]  # [3n] the original triangle each corner came from
    p = co[loop_v[corners]].astype(np.float64)
    P = src.reg(0)[:, :3].astype(np.float64)
    T = src.indices.astype(np.int64)
    tn = np.cross(P[T[:, 1]] - P[T[:, 0]], P[T[:, 2]] - P[T[:, 0]])
    area = np.linalg.norm(tn, axis=1)
    flat = area < 1e-10 * float(np.ptp(P, 0).max() or 1.0) ** 2  # no area: no place to take a texture from
    tn /= np.maximum(area[:, None], 1e-12)
    around = neighbours(src, rl)
    cn = loop_n[corners].astype(np.float64)
    tri_of = from_tri.copy()
    bc = np.zeros((len(corners), 3))
    order = np.argsort(from_tri, kind="stable")
    bounds = np.searchsorted(from_tri[order], np.arange(len(T) + 1))
    for t in range(len(T)):
        sel = order[bounds[t]:bounds[t + 1]]
        if not len(sel):
            continue
        q = p[sel]
        best = np.full(len(sel), np.inf)
        own = np.sign(cn[sel] @ tn[t])  # (the corner's normal against its own triangle's winding)
        own = np.where(own == 0, 1.0, own)
        for c in [t] + [c for c in around[t] if c != t and not flat[c]]:
            w, d = closest(q, np.repeat(P[T[c, :1]], len(sel), 0), np.repeat(P[T[c, 1:2]], len(sel), 0),
                           np.repeat(P[T[c, 2:]], len(sel), 0))
            # (not a face turned away from the corner: a thin part's other side), its own always allowed
            if c != t:
                d = np.where((cn[sel] @ tn[c]) * own > 0.2, d, np.inf)
            better = d < best - 1e-9 if c != t else d < best
            best = np.where(better, d, best)
            tri_of[sel[better]] = c
            bc[sel[better]] = w[better]
    orig_tri = T[tri_of]  # [3n, 3] the original's corners (vertex indices)
    nregs = len(regs)
    rows = np.zeros((len(corners), nregs, 4), np.float32)
    nearest = orig_tri[np.arange(len(corners)), bc.argmax(1)]
    idx_reg = next((r for r in regs if rl[r] == "indices"), None)
    w_reg = next((r for r in regs if rl[r] == "weights"), None)
    for ri, r in enumerate(regs):
        vals = src.reg(r)
        if rl[r] == "position":
            rows[:, ri, :3] = p
            rows[:, ri, 3] = vals[nearest, 3]
        elif rl[r] == "normal":
            n = loop_n[corners]
            rows[:, ri, :3] = n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-8)
            rows[:, ri, 3] = vals[nearest, 3]
        elif rl[r] == "indices":
            rows[:, ri] = vals[nearest]
        elif rl[r] == "weights":
            rows[:, ri] = vals[nearest]
        else:
            rows[:, ri] = (vals[orig_tri] * bc[:, :, None]).sum(1)
    # bone weights summed by bone over the three corners, the 3 strongest kept
    if idx_reg is not None and w_reg is not None:
        ii, wi = regs.index(idx_reg), regs.index(w_reg)
        I, W = src.reg(idx_reg)[:, :3], src.reg(w_reg)[:, :3]
        for n in range(len(corners)):
            acc = {}
            for k in range(3):
                o = orig_tri[n, k]
                for j in range(3):
                    if W[o, j] > 0:
                        b = float(I[o, j])
                        acc[b] = acc.get(b, 0.0) + float(W[o, j]) * float(bc[n, k])
            top = sorted(acc.items(), key=lambda t: -t[1])[:3]
            total = sum(w for _, w in top) or 1.0
            rows[n, ii, :3] = 0
            rows[n, wi, :3] = 0
            for j, (b, w) in enumerate(top):
                rows[n, ii, j] = b
                rows[n, wi, j] = w / total
            if not top:
                rows[n, wi, 0] = 1.0

    # corners alike made one vertex
    keys = np.round(rows.reshape(len(corners), -1), 4)
    uniq, first, inverse = np.unique(keys, axis=0, return_index=True, return_inverse=True)
    m = fhm.Mesh()
    m.key, m.tex, m.prim, m.count, m.flags, m.regs = src.key, src.tex, src.prim, src.count, src.flags, src.regs
    m.vertices = rows[first]
    m.indices = inverse.ravel().astype(np.uint32).reshape(-1, 3)
    # each vertex's original one it lies nearest (the game's pieces left out by the original's: WeaponPieces)
    m.source = nearest[first].astype(np.uint32)
    return m


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--levels", type=int, default=2)
    ap.add_argument("--blend", default="")
    ap.add_argument("--crease-angle", type=float, default=0.0, help="hard surfaces: folds over this many degrees stay sharp")
    a = ap.parse_args(argv)
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o)
    src = fhm.read(a.src)
    obj = build(src, f"{src.key:016x}", a.crease_angle)
    if a.levels > 0:
        apply_subdivision(obj, a.levels)
    m = export(obj, src)
    fhm.write(a.out, m)
    if a.blend:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(a.blend))
    print(f"wrote {a.out}: {len(m.vertices)} vertices, {len(m.indices)} triangles")


if __name__ == "__main__":
    main()
