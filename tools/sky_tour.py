"""The sky of each level from above its walls: the free camera (game_info.cpp "freecam") lifted and turned to the
level's light, and a second view higher up the sky; work/sky/lvNN.png and a sheet of all, work/sky/all.png.

    python tools/sky_tour.py [--up 4000] [--env NAME=VALUE] [levels...]
"""
import argparse
import os
import subprocess
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHOTS = os.path.join(ROOT, "work", "shots")
LEVELS = [1, 2, 4, 5, 7, 8, 11, 12, 14, 15, 17, 18]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--up", type=float, default=4000)
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("levels", nargs="*", type=int)
    a = ap.parse_args()
    out = os.path.join(ROOT, "work", "sky")
    os.makedirs(out, exist_ok=True)
    tiles = []
    for level in a.levels or LEVELS:
        tag = "sky%02d" % level
        args = [sys.executable, os.path.join(ROOT, "tools", "drive.py"), "--press", "5:START 8:A 12:A 15:A 18:A",
                "--cmd", "10:level %d" % level, "--cmd", "22:freecam %g -6 sun" % a.up, "--cmd",
                "25:freecam %g -30 sun" % a.up, "--shots", "24.5,27", "--tag", tag, "--until", "28", "--env",
                "XBE_SKIP_MOVIES=1"]
        for e in a.env:
            args += ["--env", e]
        subprocess.run(args, capture_output=True)
        sheet = Image.new("RGB", (1920, 540))
        for i, t in enumerate(["24.5", "27"]):
            try:
                sheet.paste(Image.open(os.path.join(SHOTS, "%s_%s.png" % (tag, t))).resize((960, 540)), (i * 960, 0))
            except OSError:
                pass
        sheet.save(os.path.join(out, "lv%02d.png" % level))
        tiles.append(sheet.resize((960, 270)))
        print("level", level, flush=True)
    allsheet = Image.new("RGB", (1920, 270 * ((len(tiles) + 1) // 2)))
    for i, t in enumerate(tiles):
        allsheet.paste(t, ((i % 2) * 960, (i // 2) * 270))
    allsheet.save(os.path.join(out, "all.png"))


if __name__ == "__main__":
    main()
