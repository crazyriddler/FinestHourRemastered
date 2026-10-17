"""A level at a few moments with the shadows off and on, side by side: work/shadows/lvNN.png (left off, right on).
Movies are skipped (XBE_SKIP_MOVIES), so the play starts some 18 seconds in.

    python tools/shadow_compare.py [--times 25,35] [--quality 3] [--debug N] [levels...]
"""
import argparse
import os
import shutil
import subprocess
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOML = os.path.join(ROOT, "out", "build", "release", "codfh.toml")
BAK = os.path.join(ROOT, "work", "codfh.toml.bak")
SHOTS = os.path.join(ROOT, "work", "shots")


def run(level, tag, quality, times, env, press):
    shutil.copy(BAK, TOML)
    with open(TOML, "a") as f:
        f.write("shadow_quality = %d\n" % quality)
    args = [sys.executable, os.path.join(ROOT, "tools", "drive.py"), "--press", "5:START 8:A 12:A 15:A 18:A" + press,
            "--cmd", "10:level %d" % level, "--shots", ",".join(str(t) for t in times), "--tag", tag,
            "--until", str(times[-1] + 1), "--env", "XBE_SKIP_MOVIES=1"]
    for e in env:
        args += ["--env", e]
    subprocess.run(args, capture_output=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--times", default="25,35")
    ap.add_argument("--quality", type=int, default=3)
    ap.add_argument("--debug", type=int, default=0)
    ap.add_argument("--press", default="", help="more presses, e.g. ' 24:A'")
    ap.add_argument("levels", nargs="*", type=int)
    a = ap.parse_args()
    times = [float(t) for t in a.times.split(",")]
    out = os.path.join(ROOT, "work", "shadows")
    os.makedirs(out, exist_ok=True)
    for level in a.levels or [4]:
        env = ["XBE_SHADOW_STATS=1"] + (["XBE_SHADOW_DEBUG=%d" % a.debug] if a.debug else [])
        run(level, "so%02d" % level, 0, times, [], a.press)
        run(level, "sn%02d" % level, a.quality, times, env, a.press)
        with open(os.path.join(ROOT, "out", "build", "release", "logs", "codfh.log"), errors="replace") as f:
            lines = [l for l in f if "shadows:" in l]
        with open(os.path.join(out, "lv%02d.txt" % level), "w") as f:
            f.writelines(lines)
        sheet = Image.new("RGB", (1920, 540 * len(times)))
        for i, t in enumerate(times):
            for j, tag in enumerate(["so", "sn"]):
                try:
                    im = Image.open(os.path.join(SHOTS, "%s%02d_%g.png" % (tag, level, t))).resize((960, 540))
                    sheet.paste(im, (j * 960, i * 540))
                except OSError:
                    pass
        sheet.save(os.path.join(out, "lv%02d.png" % level))
        print("level", level, flush=True)
    shutil.copy(BAK, TOML)


if __name__ == "__main__":
    main()
