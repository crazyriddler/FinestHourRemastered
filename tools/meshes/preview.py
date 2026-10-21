"""Side-by-side pictures of .fhm meshes (an original and its remaster), solid shaded from a few sides.

    blender -b --factory-startup --python tools/meshes/preview.py -- <out_prefix> <a.fhm> [<b.fhm>...] [--views 4]
"""
import argparse
import math
import os
import sys

import bpy
import numpy as np
from mathutils import Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fhm  # noqa: E402


def load(path, name):
    m = fhm.read(path)
    p = m.reg(0)[:, :3]
    n = m.reg(3)[:, :3]
    t = m.indices
    t = t[(t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])]
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(p.tolist(), [], t.tolist())
    mesh.update()
    mesh.shade_smooth()
    mesh.normals_split_custom_set_from_vertices(n.tolist())
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    return obj, p


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix")
    ap.add_argument("meshes", nargs="+")
    ap.add_argument("--views", type=int, default=4)
    ap.add_argument("--wire", action="store_true")
    ap.add_argument("--zoom", type=float, default=1.0, help="closer by this much")
    ap.add_argument("--at", type=float, nargs=3, default=None, help="the point looked at (mesh space)")
    a = ap.parse_args(argv)
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o)
    objs = []
    span = None
    for i, path in enumerate(a.meshes):
        obj, p = load(path, f"m{i}")
        lo, hi = p.min(0), p.max(0)
        span = hi - lo if span is None else np.maximum(span, hi - lo)
        objs.append((obj, lo, hi))
    # one above the other (they are drawn each alone, the pictures joined)
    axis = int(np.argmin(span))
    gap = 0.0
    for i, (obj, lo, hi) in enumerate(objs):
        off = [0.0, 0.0, 0.0]
        off[axis] = i * gap - (lo[axis] + hi[axis]) / 2
        obj.location = off
    centre = Vector([0, 0, 0])
    lo = np.min([o[1] for o in objs], 0)
    hi = np.max([o[2] for o in objs], 0)
    c = (lo + hi) / 2
    c[axis] = gap * (len(objs) - 1) / 2
    centre = Vector(c.tolist())
    radius = float(np.linalg.norm(hi - lo)) / 2 + gap * len(objs) / 2

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.color_type = "SINGLE"
    scene.display.shading.show_cavity = True
    if a.wire:
        scene.display.shading.show_object_outline = True
    scene.render.resolution_x = 1600
    scene.render.resolution_y = 900
    cam_data = bpy.data.cameras.new("cam")
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = radius * 2.1 / a.zoom
    if a.at:
        centre = Vector(a.at)
    cam = bpy.data.objects.new("cam", cam_data)
    scene.collection.objects.link(cam)
    scene.camera = cam
    for v in range(a.views):
        yaw = 2 * math.pi * v / a.views + 0.4
        d = Vector((math.cos(yaw), math.sin(yaw), 0.45)).normalized()
        cam.location = centre + d * radius * 4
        cam.rotation_euler = (centre - cam.location).to_track_quat("-Z", "Z").to_euler()
        for i, (obj, _, _) in enumerate(objs):
            for j, (o, _, _) in enumerate(objs):
                o.hide_render = i != j
            scene.render.filepath = os.path.abspath(f"{a.prefix}_{v}_{i}.png")
            bpy.ops.render.render(write_still=True)


main()
