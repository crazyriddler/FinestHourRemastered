"""Each baked level checked at its start: what the bake has (XBE_BAKE=3, the occlusion shown: grey where it has
nothing), then the frame's GPU time and a shot with Baked Lighting on and off. Shots in work/shots/bk_<level>_*.png,
a sheet of the three in work/bake/check/lvNN.png, the times printed.

    python tools/bake/bake_check.py [levels ...] [--at 60]
"""
import argparse
import os
import re
import subprocess
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from level_review import EXE_DIR, read_log  # noqa: E402

LEVELS = [1, 2, 4, 5, 7, 8, 11, 12, 14, 15, 17, 18]
START = "5:START 8:A 12:A 15:A 18:A"


def run(level, tag, at, env, cmds, shots):
    args = [sys.executable, os.path.join(ROOT, "tools", "drive.py"), "--press", START, "--cmd", "10:level %d" % level, "--tag", tag,
            "--until", str(at + 22), "--full", "--env", "XBE_SKIP_MOVIES=1"]
    for c in cmds:
        args += ["--cmd", c]
    if shots:
        args += ["--shots", ",".join(str(s) for s in shots)]
    for e in env:
        args += ["--env", e]
    subprocess.run(args, capture_output=True)
    return read_log()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("levels", nargs="*", type=int)
    ap.add_argument("--at", type=float, default=60)
    a = ap.parse_args()
    out = os.path.join(ROOT, "work", "bake", "check")
    os.makedirs(out, exist_ok=True)
    for level in a.levels or LEVELS:
        at = a.at
        run(level, "bk_%02d_cov" % level, at, ["XBE_BAKE=3", "XBE_SCENE_DEBUG=1"], ["%g:god on" % (at - 2)], [at + 2])
        log = run(level, "bk_%02d" % level, at, [], ["%g:god on" % (at - 2), "%g:perf start" % at, "%g:perf stop on" % (at + 8),
                                                     "%g:fx baked_lighting 0" % (at + 9), "%g:perf start" % (at + 11),
                                                     "%g:perf stop off" % (at + 19)], [at + 2, at + 12])
        times = dict(re.findall(r"perf \[(on|off)\]: GPU average: ([\d.]+) ms", log))
        loaded = re.search(r"bake: level %d, (\d+) cells.*sun (\w+)" % level, log)
        print("level %2d: GPU %s ms baked, %s ms not; %s" % (level, times.get("on", "?"), times.get("off", "?"),
                                                           "%s cells, sun %s" % loaded.groups() if loaded else "no bake loaded"), flush=True)
        shots = [os.path.join(ROOT, "work", "shots", n) for n in ("bk_%02d_cov_%g.png" % (level, at + 2), "bk_%02d_%g.png" % (level, at + 2),
                                                                   "bk_%02d_%g.png" % (level, at + 12))]
        ims = [Image.open(p).convert("RGB").resize((960, 540)) for p in shots if os.path.exists(p)]
        if ims:
            sheet = Image.new("RGB", (960 * len(ims), 540))
            for i, im in enumerate(ims):
                sheet.paste(im, (960 * i, 0))
            sheet.save(os.path.join(out, "lv%02d.png" % level))


if __name__ == "__main__":
    main()
