"""Every captured level baked (tools/bake/bake.exe at low priority): work/bake/cap/lvNN.geo to work/bake/out/lvNN.bake (what
the releases ship) and a copy beside the build's game (out/build/release/bake). With the level's colours captured
(work/bake/col/lvNN.col: python tools/bake/bake_tour.py --colour) the bounced light too, and the reflections' probes
(lvNN.refl).

    python tools/bake/bake_all.py [levels ...]

The captures: python tools/freecam_tour.py <level> with XBE_BAKE_CAPTURE=<cod-native>/work/bake/cap (every checkpoint
looked at from all sides; the draws, the eye's way, the soldiers' places and the level's sun written there).
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CAP = os.path.join(ROOT, "work", "bake", "cap")
COL = os.path.join(ROOT, "work", "bake", "col")
OUT = os.path.join(ROOT, "work", "bake", "out")
GAME = os.path.join(ROOT, "out", "build", "release", "bake")
BAKE = os.path.join(ROOT, "tools", "bake", "bake.exe")


def main():
    want = {int(a) for a in sys.argv[1:]}
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(GAME, exist_ok=True)
    for name in sorted(os.listdir(CAP)):
        m = re.match(r"lv(\d\d)\.geo$", name)
        if not m or (want and int(m.group(1)) not in want):
            continue
        level = m.group(1)
        out = os.path.join(OUT, "lv%s.bake" % level)
        col = os.path.join(COL, "lv%s.col" % level)
        extra = ["--colours", col] if os.path.exists(col) else []
        r = subprocess.run([BAKE, os.path.join(CAP, name), out] + extra, capture_output=True, text=True,
                           creationflags=0x00004000)  # (BELOW_NORMAL_PRIORITY_CLASS)
        last = [l for l in r.stdout.splitlines() if "static draws" in l or "sides:" in l or "sun " in l]
        print("level %s: %s" % (level, " | ".join(last) if r.returncode == 0 else "FAILED " + r.stdout[-300:]), flush=True)
        if r.returncode == 0:
            shutil.copy2(out, os.path.join(GAME, "lv%s.bake" % level))
        if not extra:
            print("level %s: no colours (%s): no bounced light, no reflections' probes" % (level, col), flush=True)
            continue
        refl = os.path.join(OUT, "lv%s.refl" % level)
        r = subprocess.run([BAKE, os.path.join(CAP, name), refl, "--probes", col], capture_output=True, text=True,
                           creationflags=0x00004000)
        last = [l for l in r.stdout.splitlines() if "probes," in l]
        print("level %s: %s" % (level, " | ".join(last) if r.returncode == 0 else "FAILED " + r.stdout[-300:]), flush=True)
        if r.returncode == 0:
            shutil.copy2(refl, os.path.join(GAME, "lv%s.refl" % level))


if __name__ == "__main__":
    main()
