"""A close textured look at part of a mesh (unlit, the texture as the game maps it, linear filtering with mips as the
GPU would): a camera at <centre> looking back along <direction>, <size> wide, for each mesh given.

    blender -b --factory-startup --python tools/meshes/uv_closeup.py -- <out prefix> <texture.png> <cx,cy,cz>
        <dx,dy,dz> <size> <a.fhm> [<b.fhm>...]
"""
import os
import sys

import bpy
from mathutils import Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uv_preview  # noqa: E402


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    out, texpath, c, d, size = argv[:5]
    meshes = argv[5:]
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    engines = [e.identifier for e in bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items]
    sc.render.engine = "BLENDER_EEVEE_NEXT" if "BLENDER_EEVEE_NEXT" in engines else "BLENDER_EEVEE"
    sc.render.resolution_x = sc.render.resolution_y = 900
    sc.view_settings.view_transform = "Standard"
    world = bpy.data.worlds.new("w")
    world.color = (0.02, 0.02, 0.02)
    sc.world = world
    img = bpy.data.images.load(texpath)
    obs = [uv_preview.load(p, "m%d" % i, img) for i, p in enumerate(meshes)]
    for ob, _ in obs:
        for mat in ob.data.materials:
            for n in mat.node_tree.nodes:
                if n.type == "TEX_IMAGE":
                    n.interpolation = "Linear"
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    sc.collection.objects.link(cam)
    sc.camera = cam
    cam.data.type = "ORTHO"
    cam.data.ortho_scale = float(size)
    cam.data.clip_end = 1000
    centre = Vector([float(x) for x in c.split(",")])
    di = Vector([float(x) for x in d.split(",")]).normalized()
    cam.location = centre + di * 50
    cam.rotation_euler = (-di).to_track_quat("-Z", "Y").to_euler()
    for k, (ob, _) in enumerate(obs):
        for o2, _ in obs:
            o2.hide_render = o2 is not ob
        sc.render.filepath = "%s_%d.png" % (out, k)
        bpy.ops.render.render(write_still=True)


if __name__ == "__main__":
    main()
