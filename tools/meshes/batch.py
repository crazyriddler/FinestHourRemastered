"""Every dumped mesh (.fhm) remastered through tools/meshes/subdivide.py (Blender), into the pack's meshes folder.

By its texture's class (the pack's materials.json): skin and cloth (characters, arms, hands) subdivide round; the
rest (weapons, vehicles, objects, the level's own models) as hard surfaces, folds over --crease-angle kept sharp. A
mesh with little to round (its faces meet flat or at hard corners: walls, boxes, steps) is left as it is: subdivided
it would only cost triangles. Characters take two levels of subdivision, the rest one; the ground, leaves, glass,
paper, effects and the sky none.

    python tools/meshes/batch.py <dump folder> [--out work/remaster/pack/meshes] [--only key,key | @file] [--jobs 8]
        [--force] (again, those done already too)
"""
import argparse
import re
import concurrent.futures
import glob
import json
import os
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fhm  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BLENDER = os.path.join(os.path.dirname(ROOT), "tools", "blender", "blender.exe")
ORGANIC_BONES = 8     # a mesh skinned to this many bones or more: arms and hands, bodies
ORGANIC_CLASSES = {"skin", "fabric"}
# left as they are: the ground and snow (subdivided they would part from the game's own floor, feet sinking or
# floating), leaves and grass (cards), glass panes and paper (flat), effects, the sky, the 2D
EXCLUDED_CLASSES = {"ground", "foliage", "glass", "paper", "effect", "sky", "ui"}
MIN_VERTICES = 100
ROUNDABLE = 0.12      # the share of its edges folded softly (5 to 55 degrees) a mesh needs to be subdivided
LARGE = 3000          # vertices: one level of subdivision past this
FLAT = 0.02           # an organic mesh's share below this: flat (a grid of water, a flag)
WATER = re.compile(r"water", re.I)  # the water's own meshes (their texture's name): the water effects draw them
# left as they are by their texture's name: the boats' hulls (low hard planks, long fans of thin triangles and
# vertices on other edges: rounded, their decks folded at the bow)
AS_THEY_ARE = re.compile(r"^(VT_Volgaboat_hull|VT_Volgaboat_innerhull|VT_Steamboat|VT_Tugboat|VT_Rowboat|VT_backgroundBoat)", re.I)


def bones(m):
    rl = m.reg_list()
    if 1 not in rl or 2 not in rl:
        return 0
    w, b = m.reg(1)[:, :3], m.reg(2)[:, :3]
    if np.abs(b - np.round(b)).max() > 1e-4:
        return 0
    return len(set(np.round(b[w > 0]).astype(int).tolist()))


def roundable(m):
    """the share of the mesh's inner edges where its faces meet softly (a curve drawn in facets)"""
    p = m.reg(0)[:, :3]
    _, wid = np.unique(np.round(p, 3), axis=0, return_inverse=True)
    wid = wid.ravel()
    t = wid[m.indices]
    t = t[(t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])]
    if not len(t):
        return 0.0
    P = np.zeros((wid.max() + 1, 3))
    P[wid] = p
    n = np.cross(P[t[:, 1]] - P[t[:, 0]], P[t[:, 2]] - P[t[:, 0]])
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)
    edges = {}
    for fi, (a, b, c) in enumerate(t):
        for u, v in ((a, b), (b, c), (c, a)):
            edges.setdefault((min(u, v), max(u, v)), []).append(fi)
    soft = inner = 0
    for faces in edges.values():
        if len(faces) != 2:
            continue
        inner += 1
        fold = np.degrees(np.arccos(float(np.clip(np.dot(n[faces[0]], n[faces[1]]), -1, 1))))
        if 5 <= fold <= 55:
            soft += 1
    return soft / inner if inner else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--out", default=os.path.join(ROOT, "work", "remaster", "pack", "meshes"))
    ap.add_argument("--crease-angle", type=float, default=70)
    ap.add_argument("--only", default="")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    if a.only.startswith("@"):  # (a file of keys, "," or a line between: a long list)
        a.only = ",".join(open(a.only[1:]).read().split())
    only = {k.strip() for k in a.only.split(",") if k.strip()} if a.only else None
    classes = {}
    mats_path = os.path.join(ROOT, "work", "remaster", "pack", "materials.json")
    if os.path.exists(mats_path):
        classes = {k: v.get("cls") for k, v in json.load(open(mats_path)).items()}
    names = {}
    index_path = os.path.join(ROOT, "work", "remaster", "extract", "index.json")
    if os.path.exists(index_path):
        names = {k: (v.get("names") or [""])[0] for k, v in json.load(open(index_path)).items()}
    meshes = {}
    for path in sorted(glob.glob(os.path.join(a.dump, "*.fhm"))):
        key = os.path.splitext(os.path.basename(path))[0]
        if only and key not in only:
            continue
        if not a.force and not only and os.path.exists(os.path.join(a.out, key + ".fhm")):
            continue
        meshes[path] = fhm.read(path)
    # the arms' and bodies' textures: their other pieces are skin and cloth as well
    skin = {m.tex for m in meshes.values() if bones(m) >= ORGANIC_BONES}

    def one(item):
        path, m = item
        key = os.path.splitext(os.path.basename(path))[0]
        name = f"{key} tex {m.tex:016x} {len(m.vertices)} vertices"
        size = m.reg(0)[:, :3].max(0) - m.reg(0)[:, :3].min(0)
        cls = classes.get(f"{m.tex:016x}")
        organic = m.tex in skin or cls in ORGANIC_CLASSES
        if WATER.search(names.get(f"{m.tex:016x}", "")):
            return f"{name}: left as it is (water)"
        if AS_THEY_ARE.search(names.get(f"{m.tex:016x}", "").strip()):
            return f"{name}: left as it is (by its name)"
        if cls in EXCLUDED_CLASSES and m.tex not in skin:
            return f"{name}: left as it is ({cls})"
        # (an arm's or a body's small piece is subdivided all the same: beside the others it would show its facets)
        if (len(m.vertices) < MIN_VERTICES and not organic) or not m.tex or size.min() < 1.0:
            return f"{name}: left as it is (small or flat)"
        share = roundable(m)
        # (skinned alone is not round: the river's waves and a flag are flat grids moved by bones)
        if share < (FLAT if organic else ROUNDABLE):
            return f"{name}: left as it is (nothing to round, {share:.2f})"
        # (characters and arms two levels; hard surfaces, the level's own models and anything large one)
        levels = 2 if organic and len(m.vertices) <= LARGE else 1
        args = [BLENDER, "-b", "--factory-startup", "--python", os.path.join(ROOT, "tools", "meshes", "subdivide.py"), "--",
                path, os.path.join(a.out, key + ".fhm"), "--levels", str(levels)]
        if not organic:
            args += ["--crease-angle", str(a.crease_angle)]
        r = subprocess.run(args, capture_output=True, text=True)
        said = [l for l in r.stdout.splitlines() if l.startswith("wrote") or "Error" in l]
        kind = "organic" if organic else "hard"
        return f"{name} {kind} {share:.2f}: {said[-1] if said else r.stderr[-300:]}"

    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        for line in pool.map(one, meshes.items()):
            print(line, flush=True)


if __name__ == "__main__":
    main()
