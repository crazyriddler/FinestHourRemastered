"""Which texture draws a region: in one run, for each bit of the textures' index (a list's order), those with the bit
set hidden (hidelist) for a screenshot between two with all shown; where the region changes, the bit is set. Prints
the texture whose bits match.

    python tools/tex_bisect.py <level> <tex list> <t0> <x0,y0,x1,y1> [press ...]
"""
import os
import subprocess
import sys

from PIL import Image, ImageStat

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(ROOT, "work")


def main():
    level, lst, t0, box = sys.argv[1], sys.argv[2], float(sys.argv[3]), [int(v) for v in sys.argv[4].split(",")]
    extra = sys.argv[5:]
    ids = [l.strip() for l in open(lst) if l.strip()]
    bits = max(1, (len(ids) - 1).bit_length())
    cmds, shots = [], []
    t = t0
    for b in range(bits):
        path = os.path.join(WORK, "bisect_%d.txt" % b)
        with open(path, "w") as f:
            f.write("\n".join(i for k, i in enumerate(ids) if (k >> b) & 1) + "\n")
        shots.append(t)
        cmds += ["--cmd", "%.2f:hidelist %s" % (t + 0.15, path.replace("\\", "/"))]
        shots.append(t + 0.4)
        cmds += ["--cmd", "%.2f:showtex" % (t + 0.55)]
        t += 0.8
    shots.append(t)
    press = ["5:START", "8:A", "12:A", "15:A", "18:A"] + extra
    args = [sys.executable, os.path.join(ROOT, "tools", "drive.py"), "--env", "XBE_SKIP_MOVIES=1", "--press", " ".join(press),
            "--cmd", "10:level %s" % level] + cmds + ["--shots", ",".join("%.2f" % s for s in shots), "--until", "%.2f" % (t + 0.5),
                                                      "--full", "--tag", "bis"]
    subprocess.run(args, capture_output=True)

    def lum(s):
        p = os.path.join(WORK, "shots", "bis_%g.png" % float("%.2f" % s))
        return ImageStat.Stat(Image.open(p).convert("L").crop(box)).mean[0]

    index = 0
    for b in range(bits):
        before, hidden, after = lum(shots[2 * b]), lum(shots[2 * b + 1]), lum(shots[2 * b + 2])
        base = (before + after) / 2
        change = abs(hidden - base)
        noise = abs(after - before)
        on = change > max(1.5, 3 * noise)
        print("bit %d: shown %.1f / %.1f, hidden %.1f -> %s" % (b, before, after, hidden, "set" if on else "-"))
        if on:
            index |= 1 << b
    print("index %d: %s" % (index, ids[index] if index < len(ids) else "?"))


if __name__ == "__main__":
    main()
