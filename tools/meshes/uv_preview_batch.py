"""uv_preview.py for many meshes in one Blender: each mesh before and after (two folders of .fhm), textured, from
above and from the side, 384 pixels: <out>/<key>_<view>_<0 before | 1 after>.png

    blender -b --factory-startup --python tools/meshes/uv_preview_batch.py -- <list.txt> <before dir> <after dir>
        <extract dir> <out dir>
list.txt: "<key> <texture id>" lines
"""
import math
import os
import sys

import bpy
from mathutils import Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np  # noqa: E402

import uv_preview  # noqa: E402  (its load())


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    lst, before, after, extract, out = argv[:5]
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    engines = [e.identifier for e in bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items]
    sc.render.engine = "BLENDER_EEVEE_NEXT" if "BLENDER_EEVEE_NEXT" in engines else "BLENDER_EEVEE"
    sc.render.resolution_x = sc.render.resolution_y = 384
    sc.view_settings.view_transform = "Standard"
    world = bpy.data.worlds.new("w")
    world.color = (0.2, 0.2, 0.2)
    sc.world = world
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    sc.collection.objects.link(cam)
    sc.camera = cam
    cam.data.type = "ORTHO"
    for line in open(lst):
        p = line.split()
        if len(p) < 2:
            continue
        key, tex = p[0], p[1]
        if os.path.exists(os.path.join(out, "%s_1_1.png" % key)):
            continue
        texpath = os.path.join(extract, tex + ".png")
        if not os.path.exists(texpath):
            continue
        img = bpy.data.images.load(texpath)
        obs = [uv_preview.load(os.path.join(before, key + ".fhm"), "b", img),
               uv_preview.load(os.path.join(after, key + ".fhm"), "a", img)]
        pts = obs[0][1]
        ctr = (pts.min(0) + pts.max(0)) / 2
        size = float(np.ptp(pts, axis=0).max()) or 1.0
        cam.data.ortho_scale = size * 1.1
        cam.data.clip_end = size * 100
        for vi, (yaw, pitch) in enumerate([(30, 60), (210, 25)]):
            d = Vector((math.cos(math.radians(pitch)) * math.sin(math.radians(yaw)), math.sin(math.radians(pitch)),
                        math.cos(math.radians(pitch)) * math.cos(math.radians(yaw))))
            cam.location = Vector(ctr.tolist()) + d * size * 3
            cam.rotation_euler = (-d).to_track_quat("-Z", "Y").to_euler()
            for k, (ob, _) in enumerate(obs):
                for o2, _ in obs:
                    o2.hide_render = o2 is not ob
                sc.render.filepath = os.path.join(out, "%s_%d_%d.png" % (key, vi, k))
                bpy.ops.render.render(write_still=True)
        for ob, _ in obs:
            me, mats = ob.data, list(ob.data.materials)
            bpy.data.objects.remove(ob)
            bpy.data.meshes.remove(me)
            for mt in mats:
                bpy.data.materials.remove(mt)
        bpy.data.images.remove(img)


main()
