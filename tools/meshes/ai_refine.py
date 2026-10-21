"""A mesh (.fhm dump) remastered with the AI: cut into sections along its length (each refined at the AI's full
resolution), UltraShape 1.0 refines each from a picture of it (docker image fh-ultrashape,
tools/meshes/ai/Dockerfile.ultrashape), and tools/meshes/refine_apply.py carries the result onto the subdivided
original.

    python tools/meshes/ai_refine.py <in.fhm> <out.fhm> [--work work/ai/<key>] [--section 30] [--margin 4]
        [--organic] [--steps 30] [--skip-ai]
"""
import argparse
import json
import math
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fhm  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BLENDER = os.path.join(os.path.dirname(ROOT), "tools", "blender", "blender.exe")
TOOLS = os.path.join(ROOT, "tools", "meshes")
CKPT = os.path.join(ROOT, "work", "ai", "ckpt")


def blender(script, *args):
    r = subprocess.run([BLENDER, "-b", "--factory-startup", "--python", os.path.join(TOOLS, script), "--", *args],
                       capture_output=True, text=True)
    if r.returncode != 0 or "Traceback" in r.stdout + r.stderr:
        sys.exit(f"{script} failed:\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}")
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--work", default="")
    ap.add_argument("--section", type=float, default=30, help="a section's length (cm)")
    ap.add_argument("--margin", type=float, default=4, help="overlap each side of a section (cm)")
    ap.add_argument("--organic", action="store_true", help="no folds creased (arms, hands)")
    ap.add_argument("--steps", type=int, default=30)
    ap.add_argument("--max-move", type=float, default=0.35)
    ap.add_argument("--skip-ai", action="store_true", help="the sections' AI meshes made already")
    a = ap.parse_args()
    m = fhm.read(a.src)
    key = f"{m.key:016x}"
    work = os.path.abspath(a.work or os.path.join(ROOT, "work", "ai", key))
    texture = os.path.join(ROOT, "work", "remaster", "extract", f"{m.tex:016x}.png")
    p = m.reg(0)[:, :3]
    lo, hi = p.min(0), p.max(0)
    axis = int((hi - lo).argmax())
    n = max(1, math.ceil((hi[axis] - lo[axis]) / a.section))
    step = (hi[axis] - lo[axis]) / n
    dirs = []
    jobs = []
    for i in range(n):
        core = [float(lo[axis] + i * step), float(lo[axis] + (i + 1) * step)]
        if i == 0:
            core[0] -= 1
        if i == n - 1:
            core[1] += 1
        span = [core[0] - a.margin, core[1] + a.margin]
        d = os.path.join(work, f"s{i}")
        dirs.append(d)
        if not a.skip_ai:
            blender("ai_inputs.py", os.path.abspath(a.src), texture, d, "--span", str(span[0]), str(span[1]), "--pitch", "10")
        with open(os.path.join(d, "norm.json")) as f:
            norm = json.load(f)
        norm["core"] = core
        with open(os.path.join(d, "norm.json"), "w") as f:
            json.dump(norm, f)
        rel = os.path.relpath(d, work).replace("\\", "/")
        jobs.append({"image": f"/work/{rel}/reference.png", "mesh": f"/work/{rel}/coarse.obj", "out": f"/work/{rel}/out/refined.glb"})
    with open(os.path.join(work, "jobs.json"), "w") as f:
        json.dump(jobs, f)
    if not a.skip_ai:
        print(f"{key}: {n} sections, refining", flush=True)
        cmd = ["docker", "run", "--rm", "--gpus", "all", "-v", f"{TOOLS}\\ai:/tools", "-v", f"{work}:/work", "-v", f"{CKPT}:/ckpt",
               "fh-ultrashape", "python", "/tools/refine.py", "--ckpt", "/ckpt/ultrashape_v1.pt", "--jobs", "/work/jobs.json",
               "--steps", str(a.steps), "--num_latents", "16384", "--chunk_size", "4000", "--octree_res", "512"]
        with open(os.path.join(work, "refine.log"), "w") as log:
            r = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, env={**os.environ, "MSYS_NO_PATHCONV": "1"})
        if r.returncode != 0:
            sys.exit(f"refine failed ({r.returncode}): {work}\\refine.log")
    args = [os.path.abspath(a.src), os.path.abspath(a.out), *dirs, "--max-move", str(a.max_move), "--blend", os.path.join(work, "applied.blend")]
    if a.organic:
        args += ["--crease-angle", "0"]
    out = blender("refine_apply.py", *args)
    print("\n".join(l for l in out.splitlines() if l.startswith(("align 11", "projected", "wrote"))))


if __name__ == "__main__":
    main()
