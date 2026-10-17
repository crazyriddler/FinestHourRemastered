"""The remaster pack: for each extracted texture, the AI upscaled colour and, for surfaces, normal, roughness, height
and cavity maps, compressed for the GPU, with each texture's material parameters.

    python -I generate.py <extract folder> <pack folder> [--limit N] [--only id,id] [--classes a,b]

Pack: <pack>/textures/<id>.c.dds (colour, BC7), <id>.n.dds (normal, BC5), <id>.m.dds (BC7: roughness, height,
cavity, specular mask) and <pack>/materials.json. Work files go to <pack>/work and are kept, so a run resumes."""
import argparse
import json
import os
import subprocess
import sys
import time

import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import materials  # noqa: E402
import pbr  # noqa: E402

# TEXCONV: DirectXTex's texconv.exe, by default ../tools/texconv beside the repo
TEXCONV = os.environ.get("TEXCONV") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                                                    "tools", "texconv", "texconv.exe")
MAX_SIDE = 1024  # the upscaled colour's largest side


def save(path, arr, mode):
    Image.fromarray((np.clip(arr, 0, 1) * 255 + 0.5).astype(np.uint8), mode).save(path)


def process(models, src, rid, info, cls, work):
    m = materials.params(cls)
    img = Image.open(os.path.join(src, rid + ".png")).convert("RGBA")
    rgba = np.asarray(img).astype(np.float32) / 255
    rgb, alpha = rgba[..., :3], rgba[..., 3]
    wrap = m["tiles"]
    up = models.run("upscale", rgb, wrap=wrap)
    h, w = up.shape[:2]
    if max(h, w) > MAX_SIDE:
        k = MAX_SIDE / max(h, w)
        up = np.asarray(Image.fromarray((up * 255).astype(np.uint8)).resize((int(w * k), int(h * k)), Image.LANCZOS)
                        ).astype(np.float32) / 255
        h, w = up.shape[:2]
    a = np.asarray(Image.fromarray((alpha * 255).astype(np.uint8), "L").resize((w, h), Image.LANCZOS)
                   ).astype(np.float32) / 255
    if info.get("alpha") and m.get("alpha_test"):
        a = np.clip((a - 0.5) * 4 + 0.5, 0, 1)  # keep cut-outs crisp after the soft resize
    save(os.path.join(work, rid + ".c.png"), np.dstack([up, a]), "RGBA")
    if not m["pbr"]:
        return False
    normal = models.run("normal", up, wrap=wrap)
    rough = models.run("rough", up, wrap=wrap)[..., 0]
    height = models.run("height", up, wrap=wrap)[..., 0]
    # height spread to the full range, cavity from what lies below its surroundings
    lo, hi = np.percentile(height, 1), np.percentile(height, 99)
    height = np.clip((height - lo) / max(hi - lo, 1e-3), 0, 1)
    blur = np.asarray(Image.fromarray((height * 255).astype(np.uint8), "L").filter(
        ImageFilter.GaussianBlur(max(2, w // 64)))).astype(np.float32) / 255
    cavity = np.clip(1 - (blur - height) * 2.5, 0, 1)
    spec = np.clip(1 - rough, 0, 1) * m["spec"] / max(materials.MAX_SPEC, 1e-3)
    save(os.path.join(work, rid + ".n.png"), normal, "RGB")
    save(os.path.join(work, rid + ".m.png"), np.dstack([rough, height, cavity, spec]), "RGBA")
    return True


def texconv(files, fmt, out):
    """BC compression with mips, in batches (the GPU path of texconv when it has one)."""
    for i in range(0, len(files), 64):
        cmd = [TEXCONV, "-nologo", "-y", "-m", "0", "-f", fmt, "-o", out] + files[i:i + 64]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            print(r.stdout[-2000:], r.stderr[-2000:])


def write_index(mats, pack):
    """materials.txt, what the runtime reads: id class pbr normal pom spec detail tiles, one texture a line"""
    with open(os.path.join(pack, "materials.txt"), "w") as f:
        f.write("# id class pbr normal pom spec detail tiles\n")
        for rid in sorted(mats):
            m = mats[rid]
            f.write("%s %s %d %.3f %.4f %.3f %d %d\n" % (rid, m["cls"], m["pbr"], m["normal"], m["pom"], m["spec"],
                                                         m["detail"], m["tiles"]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("pack")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--only", default="")
    ap.add_argument("--classes", default="")
    a = ap.parse_args()
    index = json.load(open(os.path.join(a.src, "index.json")))
    classes = json.load(open(os.path.join(a.src, "classes.json")))
    work = os.path.join(a.pack, "work")
    tex = os.path.join(a.pack, "textures")
    os.makedirs(work, exist_ok=True)
    os.makedirs(tex, exist_ok=True)
    mat_path = os.path.join(a.pack, "materials.json")
    mats = json.load(open(mat_path)) if os.path.exists(mat_path) else {}
    ids = sorted(index)
    if a.only:
        ids = [i for i in a.only.split(",") if i in index]
    if a.classes:
        want = set(a.classes.split(","))
        ids = [i for i in ids if classes[i]["class"] in want]
    if a.limit:
        ids = ids[:a.limit]
    models = pbr.Models()
    t0 = time.time()
    done = 0
    for n, rid in enumerate(ids):
        if os.path.exists(os.path.join(work, rid + ".c.png")) and rid in mats:
            continue
        cls = classes[rid]["class"]
        try:
            has_pbr = process(models, a.src, rid, index[rid], cls, work)
        except Exception as e:  # a texture the models cannot take stays as the game has it
            print("skip", rid, e)
            continue
        mats[rid] = dict(materials.params(cls), cls=cls, pbr=has_pbr, w=index[rid]["w"], h=index[rid]["h"])
        done += 1
        if done % 50 == 0:
            json.dump(mats, open(mat_path, "w"), indent=1, sort_keys=True)
            el = time.time() - t0
            print("%d/%d  %.1f s each" % (n + 1, len(ids), el / done), flush=True)
    json.dump(mats, open(mat_path, "w"), indent=1, sort_keys=True)
    write_index(mats, a.pack)
    # compression: everything not compressed yet
    def pending(suffix):
        return [os.path.join(work, f) for f in os.listdir(work) if f.endswith(suffix) and
                not os.path.exists(os.path.join(tex, f[:-4] + ".dds"))]
    texconv(pending(".c.png"), "BC7_UNORM", tex)
    texconv(pending(".n.png"), "BC5_UNORM", tex)
    texconv(pending(".m.png"), "BC7_UNORM", tex)
    print("done", len(mats), "materials")


if __name__ == "__main__":
    main()
