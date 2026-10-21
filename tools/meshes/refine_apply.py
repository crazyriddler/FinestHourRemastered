"""The AI refinement carried onto the game's mesh: the original subdivided (tools/meshes/subdivide.py), its long
edges split until dense, then each vertex moved along its normal onto the AI's surface, no further than --max-move.
The AI mesh is only a target: the result keeps the original's vertices' texture coordinates and bones, and where the
AI lost or invented a part (no surface within reach) the vertex stays where the subdivision put it.

    blender -b --factory-startup --python tools/meshes/refine_apply.py -- <orig.fhm> <out.fhm> <section dir>...
        [--levels 2] [--crease-angle 70] [--edge 0.6] [--max-move 0.35] [--smooth 4] [--blend out.blend]

A section's folder: tools/meshes/ai_inputs.py's (norm.json: its span along the longest axis, and the core of it whose
vertices it moves) with the AI's out/refined.glb.
"""
import argparse
import json
import math
import os
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector
from mathutils.kdtree import KDTree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fhm  # noqa: E402
import subdivide  # noqa: E402


def mesh_points(obj):
    me = obj.data
    co = np.zeros(len(me.vertices) * 3, np.float64)
    me.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3)
    m = np.array(obj.matrix_world)
    return co @ m[:3, :3].T + m[:3, 3]


def import_ai(path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path)
    new = [o for o in bpy.data.objects if o not in before and o.type == "MESH"]
    bpy.ops.object.select_all(action="DESELECT")
    for o in new:
        o.select_set(True)
    bpy.context.view_layer.objects.active = new[0]
    if len(new) > 1:
        bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    # glTF is y up, the importer turned it z up: back to the file's own axes, which are the game's
    obj.data.transform(Matrix.Rotation(-math.pi / 2, 4, "X"))
    return obj


def register(ai, target_pts, norm):
    """the AI mesh into the game's space: undo the normalisation (its loader rescales to its own bounding box, so
    the boxes are matched), then a few rounds of closest-point alignment (rotation, scale, offset)"""
    src = mesh_points(ai)
    lo, hi = src.min(0), src.max(0)
    tlo, thi = target_pts.min(0), target_pts.max(0)
    s = float((thi - tlo).max() / (hi - lo).max())
    m = Matrix.Translation(Vector(((tlo + thi) / 2).tolist())) @ Matrix.Scale(s, 4) @ Matrix.Translation(Vector((-(lo + hi) / 2).tolist()))
    ai.data.transform(m)
    tree = KDTree(len(target_pts))
    for i, p in enumerate(target_pts):
        tree.insert(p, i)
    tree.balance()
    rng = np.random.default_rng(0)
    for it in range(12):
        pts = mesh_points(ai)
        sample = pts[rng.choice(len(pts), min(len(pts), 20000), replace=False)]
        near = np.array([tree.find(p)[0] for p in sample])
        d = np.linalg.norm(near - sample, axis=1)
        keep = d < np.percentile(d, 80)
        a, b = sample[keep], near[keep]
        ca, cb = a.mean(0), b.mean(0)
        u, sv, vt = np.linalg.svd((a - ca).T @ (b - cb))
        r = (u @ vt).T
        if np.linalg.det(r) < 0:
            vt[-1] *= -1
            r = (u @ vt).T
        sc = sv.sum() / ((a - ca) ** 2).sum()
        t = cb - sc * r @ ca
        mm = Matrix.Identity(4)
        for i in range(3):
            for j in range(3):
                mm[i][j] = sc * r[i, j]
            mm[i][3] = t[i]
        ai.data.transform(mm)
        print(f"align {it}: distance median {np.median(d):.3f} p90 {np.percentile(d, 90):.3f}, scale {sc:.4f}")


def split_long_edges(obj, edge):
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    for _ in range(4):
        long = [e for e in bm.edges if e.calc_length() > edge]
        if not long:
            break
        bmesh.ops.subdivide_edges(bm, edges=long, cuts=1, use_grid_fill=True)
        bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
    bm.to_mesh(obj.data)
    bm.free()


def smooth(ai, iterations):
    """the marching cubes' steps on the AI's surface smoothed away (its features are larger)"""
    mod = ai.modifiers.new("smooth", "SMOOTH")
    mod.factor = 0.5
    mod.iterations = iterations


def project(obj, sections, axis, max_move):
    """each vertex along its normal (either way) onto its section's AI surface, if one is within max_move and faces
    the same way"""
    me = obj.data
    me.update()
    deps = bpy.context.evaluated_depsgraph_get()
    evaluated = [(core, ai.evaluated_get(deps)) for core, ai in sections]
    moved = 0
    total = 0.0
    for v in me.vertices:
        x = v.co[axis]
        ai_eval = next((e for core, e in evaluated if core[0] <= x <= core[1]), None)
        if ai_eval is None:
            continue
        n = v.normal
        best = None
        for sign in (1, -1):
            hit, loc, normal, _ = ai_eval.ray_cast(v.co, n * sign, distance=max_move)
            if hit and normal.dot(n) > 0.5 and (best is None or (loc - v.co).length < (best - v.co).length):
                best = loc
        if best is not None:
            total += (best - v.co).length
            v.co = best
            moved += 1
    print(f"projected {moved}/{len(me.vertices)} vertices (mean move {total / max(moved, 1):.3f})")


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("sections", nargs="+", help="folders of tools/meshes/ai_inputs.py (norm.json with its span and "
                                                "core) holding the AI's refined.glb")
    ap.add_argument("--levels", type=int, default=2)
    ap.add_argument("--crease-angle", type=float, default=70)
    ap.add_argument("--edge", type=float, default=0.6, help="edges longer than this (game units, cm) split")
    ap.add_argument("--max-move", type=float, default=0.35)
    ap.add_argument("--smooth", type=int, default=4)
    ap.add_argument("--blend", default="")
    a = ap.parse_args(argv)
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o)
    src = fhm.read(a.src)
    p = src.reg(0)[:, :3]
    axis = int(np.argmax(p.max(0) - p.min(0)))
    obj = subdivide.build(src, "game", a.crease_angle)
    subdivide.apply_subdivision(obj, a.levels)
    split_long_edges(obj, a.edge)
    dense = mesh_points(obj)
    sections = []
    for d in a.sections:
        with open(os.path.join(d, "norm.json")) as f:
            norm = json.load(f)
        span = norm.get("span") or [-1e9, 1e9]
        core = norm.get("core") or span
        ai = import_ai(os.path.abspath(os.path.join(d, "out", "refined.glb")))
        # (aligned to the dense mesh of its own section: the original's vertices are too sparse to measure a
        # surface by)
        register(ai, dense[(dense[:, axis] >= span[0]) & (dense[:, axis] <= span[1])], norm)
        smooth(ai, a.smooth)
        sections.append((core, ai))
    project(obj, sections, axis, a.max_move)
    m = subdivide.export(obj, src)
    fhm.write(a.out, m)
    if a.blend:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(a.blend))
    print(f"wrote {a.out}: {len(m.vertices)} vertices, {len(m.indices)} triangles")


main()
