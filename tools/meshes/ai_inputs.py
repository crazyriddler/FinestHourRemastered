"""The AI refinement's inputs for a mesh (.fhm): the mesh normalised to the unit cube (centred, longest half-side
0.95) as .obj, the normalisation in a .json, and a textured picture of it (transparent background) as the reference.

    blender -b --factory-startup --python tools/meshes/ai_inputs.py -- <in.fhm> <texture.png> <out_dir> [--yaw 90]
"""
import argparse
import json
import math
import os
import sys

import bpy
import numpy as np
from mathutils import Matrix, Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fhm  # noqa: E402


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("texture")
    ap.add_argument("out")
    ap.add_argument("--yaw", type=float, default=0, help="the view's direction about the up axis (y), degrees")
    ap.add_argument("--pitch", type=float, default=15)
    ap.add_argument("--size", type=int, default=1024)
    ap.add_argument("--span", type=float, nargs=2, default=None,
                    help="a section only: the triangles whose centre is within this range along the longest axis")
    a = ap.parse_args(argv)
    os.makedirs(a.out, exist_ok=True)
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o)

    m = fhm.read(a.src)
    p = m.reg(0)[:, :3].astype(np.float64)
    t = m.indices
    t = t[(t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])]
    if a.span:
        axis = int(np.argmax(p.max(0) - p.min(0)))
        c = p[t].mean(1)[:, axis]
        t = t[(c >= a.span[0]) & (c <= a.span[1])]
    used = np.unique(t)
    lo, hi = p[used].min(0), p[used].max(0)
    centre = (lo + hi) / 2
    scale = 0.95 / float((hi - lo).max() / 2)
    q = (p - centre) * scale
    with open(os.path.join(a.out, "norm.json"), "w") as f:
        json.dump({"centre": centre.tolist(), "scale": scale, "span": a.span}, f)
    with open(os.path.join(a.out, "coarse.obj"), "w") as f:
        new = np.full(len(q), -1)
        new[used] = np.arange(len(used))
        for v in q[used]:
            f.write("v %.6f %.6f %.6f\n" % tuple(v))
        for tri in new[t]:
            f.write("f %d %d %d\n" % (tri[0] + 1, tri[1] + 1, tri[2] + 1))

    mesh = bpy.data.meshes.new("m")
    mesh.from_pydata(q.tolist(), [], t.tolist())
    mesh.update()
    uv = m.reg(5)[:, :2]
    layer = mesh.uv_layers.new(name="uv")
    loops = np.zeros(len(mesh.loops), np.int32)
    mesh.loops.foreach_get("vertex_index", loops)
    layer.data.foreach_set("uv", uv[loops].astype(np.float32).ravel())
    mesh.shade_smooth()
    mesh.normals_split_custom_set_from_vertices(m.reg(3)[:, :3].tolist())
    obj = bpy.data.objects.new("m", mesh)
    bpy.context.collection.objects.link(obj)

    mat = bpy.data.materials.new("tex")
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes["Principled BSDF"]
    img = nt.nodes.new("ShaderNodeTexImage")
    img.image = bpy.data.images.load(os.path.abspath(a.texture))
    nt.links.new(img.outputs["Color"], bsdf.inputs["Base Color"])
    bsdf.inputs["Roughness"].default_value = 0.6
    mat.use_backface_culling = False
    obj.data.materials.append(mat)

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE_NEXT"
    scene.render.film_transparent = True
    scene.render.resolution_x = scene.render.resolution_y = a.size
    scene.world = bpy.data.worlds.new("w")
    scene.world.use_nodes = True
    scene.world.node_tree.nodes["Background"].inputs["Strength"].default_value = 1.0
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy = 3.0
    sun.rotation_euler = (math.radians(50), 0, math.radians(30))
    scene.collection.objects.link(sun)
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    cam.data.type = "ORTHO"
    cam.data.ortho_scale = 2.1
    scene.collection.objects.link(cam)
    scene.camera = cam
    # the game's bind space is y up, as the model's: the view from (yaw about y, pitch), y up in the picture
    yaw, pitch = math.radians(a.yaw), math.radians(a.pitch)
    d = Vector((math.cos(yaw) * math.cos(pitch), math.sin(pitch), math.sin(yaw) * math.cos(pitch)))
    cam.location = d * 5
    zc = d.normalized()
    xc = Vector((0, 1, 0)).cross(zc).normalized()
    yc = zc.cross(xc)
    cam.rotation_euler = Matrix((xc, yc, zc)).transposed().to_euler()
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.exposure = 1.5
    scene.render.filepath = os.path.abspath(os.path.join(a.out, "reference.png"))
    bpy.ops.render.render(write_still=True)


main()
