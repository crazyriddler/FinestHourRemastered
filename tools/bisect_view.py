"""Which texture's draws make a region of a view what it is, in one run: a level started, the free camera put at the
player's eye looking along the world's yaw and pitch given, the textures drawn listed (texview), then for each bit
of their index those with it set hidden (a hidelist) for a screenshot; where the region's brightness changes from the
shot with all shown, the bit is set. Prints the texture whose bits match, and the region's brightness each time.

    python tools/bisect_view.py <level> <yaw> <pitch> <x0,y0,x1,y1 of 640x360> [--tag name] [--at seconds]
"""
import argparse
import math
import os
import re
import subprocess
import sys
import time

from PIL import Image, ImageStat

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive  # noqa: E402
from level_review import EXE_DIR, START, WORK, read_log  # noqa: E402

SLOTS = 256  # gpu::g_hidden_tex: each hidelist fills them all, so none is left from the one before


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("level", type=int)
    ap.add_argument("yaw", type=float)
    ap.add_argument("pitch", type=float)
    ap.add_argument("box")
    ap.add_argument("--tag", default="bisect")
    ap.add_argument("--at", type=float, default=34)
    ap.add_argument("--skip", default="", help="classes left out of the search (materials.txt), e.g. ground,stone")
    ap.add_argument("--each", action="store_true", help="each texture hidden alone, a shot each")
    a = ap.parse_args()
    skip = set(c for c in a.skip.split(",") if c)
    classes = {}
    for line in open(os.path.join(EXE_DIR, "remaster", "materials.txt")):
        f = line.split()
        if len(f) > 1 and not line.startswith("#"):
            classes[f[0]] = f[1]
    box = [int(v) for v in a.box.split(",")]
    control = os.path.join(WORK, a.tag + ".control.txt")
    open(control, "w").close()
    replay = os.path.join(WORK, a.tag + ".replay.txt")
    drive.build_replay(START, replay)
    with open(replay, "a") as f:
        f.write("%.1f 0 1 0000 0 0 0 0 0 0 0 0 0 0 0 0\n" % 600000)
    drive.kill_build_exe()
    env = dict(os.environ)
    env.update({"XBE_CONTROL": control, "XBE_INPUT_REPLAY": replay, "XBE_SKIP_MOVIES": "1"})
    subprocess.Popen([os.path.join(EXE_DIR, "CODFH.exe")], cwd=EXE_DIR, env=env)
    t0 = time.time()

    def send(line):
        with open(control, "a") as f:
            f.write(line + "\n")

    def shot(name):
        bmp = os.path.join(WORK, "shots", "%s_%s.bmp" % (a.tag, name)).replace("\\", "/")
        if os.path.exists(bmp):
            os.remove(bmp)
        send("shot " + bmp)
        for _ in range(60):
            time.sleep(0.1)
            if os.path.exists(bmp) and os.path.getsize(bmp) > 1000:
                break
        time.sleep(0.2)
        im = Image.open(bmp).convert("RGB").resize((640, 360))
        os.remove(bmp)
        im.save(os.path.join(WORK, "shots", "%s_%s.png" % (a.tag, name)))
        return ImageStat.Stat(im.crop(box).convert("L")).mean[0]

    while time.time() - t0 < 10:
        time.sleep(0.05)
    send("cmd level %d" % a.level)
    while time.time() - t0 < a.at:
        time.sleep(0.05)
    send("cmd god on")
    send("cmd whereami")
    time.sleep(0.8)
    found = re.findall(r"whereami: (\S+) (\S+) (\S+) yaw", read_log())
    if not found:
        drive.kill_build_exe()
        sys.exit("no camera")
    x, y, z = map(float, found[-1])
    send("cmd freecam at %.1f %.1f %.1f %.2f %.2f" % (x, y, z, a.yaw, a.pitch))
    time.sleep(1.5)
    send("cmd texview 1")
    time.sleep(0.6)
    send("cmd texview 0")
    time.sleep(0.4)
    ids = sorted(t for t in set(re.findall(r"texview: ([0-9a-f]{16})", read_log())) if classes.get(t, "none") not in skip)
    bits = max(1, math.ceil(math.log2(len(ids) + 1)))
    lists = os.path.join(WORK, a.tag + ".hide.txt")

    def hide(chosen):
        with open(lists, "w") as f:
            for i in range(SLOTS):
                f.write("%s\n" % (chosen[i] if i < len(chosen) else "1"))
        send("cmd hidelist " + lists.replace("\\", "/"))
        time.sleep(0.5)

    base = shot("all")
    print("textures %d, all shown: %.1f" % (len(ids), base), flush=True)
    if a.each:
        for i, t in enumerate(ids):
            hide([t])
            v = shot("t%d" % i)
            print("%s %-8s %.1f%s" % (t, classes.get(t, "none"), v, "  changed" if abs(v - base) > max(2.0, base * 0.08) else ""), flush=True)
        hide([])
        drive.kill_build_exe()
        return
    code = 0
    for b in range(bits):
        hide([t for i, t in enumerate(ids) if ((i + 1) >> b) & 1])
        v = shot("bit%d" % b)
        changed = abs(v - base) > max(2.0, base * 0.08)
        print("bit %d: %.1f%s" % (b, v, "  changed" if changed else ""), flush=True)
        if changed:
            code |= 1 << b
    hide([])
    again = shot("again")
    print("all shown again: %.1f" % again, flush=True)
    drive.kill_build_exe()
    if code and code - 1 < len(ids):
        print("texture:", ids[code - 1])
    else:
        print("no single texture (code %d)" % code)


if __name__ == "__main__":
    main()
