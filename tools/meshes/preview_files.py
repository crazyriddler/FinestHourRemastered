"""Pictures of mesh files (.obj/.glb, as the AI tools read and write them), each alone from the same cameras.

    blender -b --factory-startup --python tools/meshes/preview_files.py -- <out_prefix> <a.obj> [<b.glb>...]
        [--zoom 1] [--at x y z]
"""
import argparse
import math
import os
import sys

import bpy
from mathutils import Matrix, Vector


def load(path):
    before = set(bpy.data.objects)
    if path.lower().endswith(".obj"):
        bpy.ops.wm.obj_import(filepath=path, forward_axis="NEGATIVE_Z", up_axis="Y")
    else:
        bpy.ops.import_scene.gltf(filepath=path)
    new = [o for o in bpy.data.objects if o not in before and o.type == "MESH"]
    for o in new:
        for p in o.data.polygons:
            p.use_smooth = False
    return new


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix")
    ap.add_argument("files", nargs="+")
    ap.add_argument("--zoom", type=float, default=1.0)
    ap.add_argument("--at", type=float, nargs=3, default=(0, 0, 0))
    a = ap.parse_args(argv)
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o)
    groups = [load(os.path.abspath(f)) for f in a.files]
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.show_cavity = True
    scene.render.resolution_x, scene.render.resolution_y = 1600, 900
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    cam.data.type = "ORTHO"
    cam.data.ortho_scale = 2.2 / a.zoom
    scene.collection.objects.link(cam)
    scene.camera = cam
    # the views (blender is z up: the y-up meshes' up is blender's z after import): side, top-side, back
    centre = Vector(a.at)
    for v, (yaw, pitch) in enumerate(((0, 10), (35, 45), (160, 25))):
        y, p = math.radians(yaw), math.radians(pitch)
        d = Vector((math.cos(y) * math.cos(p), math.sin(y) * math.cos(p), math.sin(p)))
        zc = d.normalized()
        xc = Vector((0, 0, 1)).cross(zc).normalized()
        yc = zc.cross(xc)
        cam.rotation_euler = Matrix((xc, yc, zc)).transposed().to_euler()
        cam.location = centre + d * 5
        for i, g in enumerate(groups):
            for j, h in enumerate(groups):
                for o in h:
                    o.hide_render = i != j
            scene.render.filepath = os.path.abspath(f"{a.prefix}_{v}_{i}.png")
            bpy.ops.render.render(write_still=True)


main()
