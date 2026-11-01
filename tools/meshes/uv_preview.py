"""Textured pictures of meshes (an original and its remaster), unlit, its texture (the extract's png, nearest
filtering) as the game maps it, from a few sides: their texture coordinates side by side.

    blender -b --factory-startup --python tools/meshes/uv_preview.py -- <out.png> <texture.png> <a.fhm> <b.fhm>
"""
import math
import os
import sys

import bpy
import numpy as np
from mathutils import Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fhm  # noqa: E402


def load(path, name, img):
    m = fhm.read(path)
    p = m.reg(0)[:, :3].astype(float)
    uv = m.reg(9)[:, :2].astype(float)
    t = m.indices
    t = t[(t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])]
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(p.tolist(), [], t.tolist())
    layer = mesh.uv_layers.new(name="uv")
    loops = np.array([v for poly in mesh.polygons for v in poly.vertices])
    co = uv[loops]
    co[:, 1] = 1 - co[:, 1]  # (D3D's v down, Blender's up)
    layer.data.foreach_set("uv", co.ravel())
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = img
    tex.interpolation = "Closest"
    em = nt.nodes.new("ShaderNodeEmission")
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    nt.links.new(tex.outputs["Color"], em.inputs["Color"])
    nt.links.new(em.outputs["Emission"], out.inputs["Surface"])
    mesh.materials.append(mat)
    ob = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(ob)
    return ob, p


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    out, texpath, a, b = argv[:4]
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    sc.render.engine = "BLENDER_EEVEE_NEXT" if "BLENDER_EEVEE_NEXT" in [e.identifier for e in bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items] else "BLENDER_EEVEE"
    sc.render.resolution_x, sc.render.resolution_y = 640, 640
    sc.view_settings.view_transform = "Standard"
    world = bpy.data.worlds.new("w")
    world.color = (0.2, 0.2, 0.2)
    sc.world = world
    img = bpy.data.images.load(texpath)
    obs = [load(a, "orig", img), load(b, "remaster", img)]
    pts = obs[0][1]
    ctr = (pts.min(0) + pts.max(0)) / 2
    size = float(np.ptp(pts, axis=0).max())
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    sc.collection.objects.link(cam)
    sc.camera = cam
    cam.data.type = "ORTHO"
    cam.data.ortho_scale = size * 1.1
    cam.data.clip_end = size * 100
    files = []
    views = [(0, 89), (45, 35), (135, 35), (225, 35), (315, 35), (0, -60)]
    for vi, (yaw, pitch) in enumerate(views):
        d = Vector((math.cos(math.radians(pitch)) * math.sin(math.radians(yaw)), math.sin(math.radians(pitch)),
                    math.cos(math.radians(pitch)) * math.cos(math.radians(yaw))))
        cam.location = Vector(ctr.tolist()) + d * size * 3
        cam.rotation_euler = (-d).to_track_quat("-Z", "Y").to_euler()
        for k, (ob, _) in enumerate(obs):
            for o2, _ in obs:
                o2.hide_render = o2 is not ob
            f = "%s_%d_%d.png" % (out[:-4], vi, k)
            sc.render.filepath = f
            bpy.ops.render.render(write_still=True)
            files.append(f)
    print("wrote", len(files))


if __name__ == "__main__":
    main()
