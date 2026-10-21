"""Pictures of a model as the game had it and as remastered (.fhm), for the README: each textured (its game texture)
and in clay with its wireframe, from the same camera.

    blender -b --factory-startup --python tools/meshes/showcase.py -- <out_prefix> <original.fhm> <remastered.fhm>
        <texture.png> [<original.fhm> <remastered.fhm> <texture.png>...] [--no-pose] [--yaw 35] [--pitch 20] [--zoom 1] [--size 1200 900]

Writes <out_prefix>_{original,remastered}_{tex,wire}.png.
"""
import argparse
import math
import os
import sys

import bpy
import numpy as np
from mathutils import Matrix, Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fhm  # noqa: E402


def unit_reg(m):
    """the register holding the normals (unit vectors), past the position"""
    for r in m.reg_list()[1:]:
        v = m.reg(r)[:, :3]
        # (not a mesh's weights, all (1, 0, 0) for one bone: unit too)
        if np.abs(np.linalg.norm(v, axis=1) - 1).mean() < 0.05 and v.std(0).sum() > 0.2:
            return r
    return None


def posed(m, consts):
    """positions and normals skinned by the dump's bones (each bone's rows at c[index + 14..16]), or as they are"""
    p = m.reg(0)[:, :3]
    n = unit_reg(m)
    nrm = m.reg(n)[:, :3] if n is not None else None
    rl = m.reg_list()
    if consts is None or 1 not in rl or 2 not in rl or n == 1 or n == 2:
        return p, nrm
    idx = m.reg(2)[:, :3]
    if np.abs(idx - np.round(idx)).max() > 1e-4:
        return p, nrm
    w, idx = m.reg(1)[:, :3], idx.astype(int)
    ph = np.concatenate([p, np.ones((len(p), 1))], 1)
    out = np.zeros((len(p), 3))
    on = np.zeros((len(p), 3))
    for k in range(3):
        base = idx[:, k] + 14 + 96
        rows = np.stack([consts[np.clip(base + r, 0, 191)] for r in range(3)], 1)  # [n, 3, 4]
        out += w[:, k:k + 1] * np.einsum("nrc,nc->nr", rows, ph)
        if nrm is not None:
            on += w[:, k:k + 1] * np.einsum("nrc,nc->nr", rows[:, :, :3], nrm)
    if nrm is not None:
        on /= np.maximum(np.linalg.norm(on, axis=1, keepdims=True), 1e-9)
    return out, on if nrm is not None else None


def load(path, name, image, consts, keep=None, offset=(0, 0, 0)):
    """the mesh as a Blender object and its wireframe copy; keep(p): the vertices kept (triangles past it dropped)"""
    m = fhm.read(path)
    p, nrm = posed(m, consts)
    p = p + np.asarray(offset)
    t = m.indices
    t = t[(t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])]
    if keep is not None:
        k = keep(p)
        t = t[k[t].all(1)]
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(p.tolist(), [], t.tolist())
    mesh.update()
    uv = m.reg(m.reg_list()[-1])[:, :2]
    layer = mesh.uv_layers.new(name="uv")
    loops = np.zeros(len(mesh.loops) * 2, np.float32)
    vi = np.zeros(len(mesh.loops), np.int64)
    mesh.loops.foreach_get("vertex_index", vi)
    loops[0::2] = uv[vi, 0]
    loops[1::2] = 1.0 - uv[vi, 1]
    layer.data.foreach_set("uv", loops)
    mesh.shade_smooth()
    if nrm is not None:
        mesh.normals_split_custom_set_from_vertices(nrm.tolist())
    mat = bpy.data.materials.new(name + "_mat")
    mat.use_nodes = True
    tex = mat.node_tree.nodes.new("ShaderNodeTexImage")
    tex.image = image
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    mat.node_tree.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    mesh.materials.append(mat)
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    # its wireframe, a copy drawn as thin dark bars
    wire = bpy.data.objects.new(name + "_wire", mesh.copy())
    wire.data.materials.clear()
    bpy.context.collection.objects.link(wire)
    mod = wire.modifiers.new("wire", "WIREFRAME")
    mod.use_replace = True
    mod.use_even_offset = False  # (mitred at slivers it would spike)
    return obj, wire, p


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix")
    ap.add_argument("pieces", nargs="+", help="original.fhm remastered.fhm texture.png, for each piece")
    ap.add_argument("--yaw", type=float, default=35)
    ap.add_argument("--pitch", type=float, default=20)
    ap.add_argument("--zoom", type=float, default=1.0)
    ap.add_argument("--no-pose", action="store_true", help="as the mesh holds it (not by the dump's bones)")
    ap.add_argument("--offset", type=float, nargs=4, action="append", default=[],
                    help="piece x y z: that piece moved (a head drawn about its own bone, set on the neck)")
    ap.add_argument("--size", type=int, nargs=2, default=(1200, 900))
    a = ap.parse_args(argv)
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o)
    pieces = [a.pieces[i:i + 3] for i in range(0, len(a.pieces), 3)]
    consts = [None if a.no_pose else fhm.read(orig).constants for orig, _, _ in pieces]
    # framed on the model itself (a skinned mesh's pieces the game hides lie far off: left out)
    offsets = [(0.0, 0.0, 0.0)] * len(pieces)
    for i, x, y, z in a.offset:
        offsets[int(i)] = (x, y, z)
    p = np.concatenate([posed(fhm.read(orig), c)[0] + np.asarray(o)
                        for (orig, _, _), c, o in zip(pieces, consts, offsets)])
    mid = np.median(p, 0)
    reach = np.percentile(np.linalg.norm(p - mid, axis=1), 97) * 1.6

    def keep(q):
        return np.linalg.norm(q - mid, axis=1) <= reach

    p = p[keep(p)]
    lo, hi = p.min(0), p.max(0)
    centre = Vector(((lo + hi) / 2).tolist())
    radius = float(np.linalg.norm(hi - lo)) / 2
    sets = {"original": [], "remastered": []}
    for i, ((orig, rem, tex), c) in enumerate(zip(pieces, consts)):
        image = bpy.data.images.load(os.path.abspath(tex))
        sets["original"].append(load(orig, f"original{i}", image, c, keep, offsets[i]))
        sets["remastered"].append(load(rem, f"remastered{i}", image, c, keep, offsets[i]))
    for objs in sets.values():
        for _, wire, _ in objs:
            wire.modifiers["wire"].thickness = radius * 0.0028 / a.zoom

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.show_cavity = False
    scene.display.shading.show_specular_highlight = True
    scene.render.resolution_x, scene.render.resolution_y = a.size
    scene.render.film_transparent = True
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.exposure = 1.3
    cam_data = bpy.data.cameras.new("cam")
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = radius * 2.05 / a.zoom
    cam = bpy.data.objects.new("cam", cam_data)
    scene.collection.objects.link(cam)
    scene.camera = cam
    yaw, pitch = math.radians(a.yaw), math.radians(a.pitch)
    # the game's up is y
    d = Vector((math.cos(yaw) * math.cos(pitch), math.sin(pitch), math.sin(yaw) * math.cos(pitch)))
    cam.location = centre + d * radius * 4
    # looking at the centre, the game's up (y) up on the picture
    fwd = (centre - cam.location).normalized()
    right = fwd.cross(Vector((0, 1, 0))).normalized()
    up = right.cross(fwd)
    cam.rotation_euler = Matrix((right, up, -fwd)).transposed().to_euler()
    cam_data.clip_end = radius * 10
    # the picture fitted to the model as the camera sees it
    q = p - np.asarray(centre)
    sx, sy = q @ np.asarray(right), q @ np.asarray(up)
    aspect = a.size[0] / a.size[1]
    cam_data.ortho_scale = max(np.ptp(sx), np.ptp(sy) * aspect) * 1.08 / a.zoom
    cam.location = cam.location + right * float(sx.max() + sx.min()) / 2 + up * float(sy.max() + sy.min()) / 2

    for name in sets:
        for kind in ("tex", "wire"):
            for other, objs in sets.items():
                for o, w, _ in objs:
                    o.hide_render = other != name
                    w.hide_render = other != name or kind != "wire"
                    # clay, and the wireframe dark over it (the object colour)
                    o.color = (0.72, 0.70, 0.66, 1)
                    w.color = (0.05, 0.05, 0.06, 1)
            scene.display.shading.color_type = "TEXTURE" if kind == "tex" else "OBJECT"
            scene.render.filepath = os.path.abspath(f"{a.prefix}_{name}_{kind}.png")
            bpy.ops.render.render(write_still=True)


main()
