"""Level 4's window (the sniper's): the camera tilted down step by step, a region's brightness at each step, with
some effects off. The largest drop between two steps tells a light that switches rather than fades.

    python tools/pitch_probe.py <tag> [effect ...]
"""
import os
import subprocess
import sys

from PIL import Image, ImageStat

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHOTS = os.path.join(ROOT, "work", "shots")


def main():
    tag, effects = sys.argv[1], sys.argv[2:]
    press = ["5:START", "8:A", "12:A", "15:A", "18:A"]
    t = 52.0
    for _ in range(14):
        press.append("%.2f:RY-/0.12" % (t + 0.2))
        t += 0.7
    times = ["%.2f" % (55 + i * 0.35) for i in range(18)]
    args = [sys.executable, os.path.join(ROOT, "tools", "drive.py"), "--env", "XBE_SKIP_MOVIES=1", "--press", " ".join(press),
            "--cmd", "10:level 4"]
    for e in effects:
        args += ["--cmd", "9:fx %s 0" % e]
    args += ["--shots", ",".join(times), "--until", "62", "--full", "--tag", "pp_" + tag]
    subprocess.run(args, capture_output=True)
    v = []
    for s in times:
        p = os.path.join(SHOTS, "pp_%s_%g.png" % (tag, float(s)))
        if not os.path.exists(p):
            continue
        im = Image.open(p).convert("L")
        w, h = im.size
        v.append(ImageStat.Stat(im.crop((int(w * 0.15), int(h * 0.5), int(w * 0.5), int(h * 0.62)))).mean[0])
    drop = max(v[i] / max(v[i + 1], 0.1) for i in range(len(v) - 1)) if len(v) > 1 else 0
    print(tag, " ".join("%.1f" % x for x in v), "| max drop x%.2f" % drop)


if __name__ == "__main__":
    main()
