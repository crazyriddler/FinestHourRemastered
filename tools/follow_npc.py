"""Follow the squad (tests): the level started at a checkpoint (drive.py, kept running), god mode on, then every few
seconds the player warped just behind the ally (actors of vtable <vt>) farthest from it, so the scripts that wait for
the player move them on; ends near the target or at the time limit, then the given commands, and the game closed.

    python tools/follow_npc.py <tag> <level> "<checkpoint cmd>" <x> <z> [--vt 002E7000] [--limit 600] [--then "a;b"]
"""
import argparse
import math
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "out", "build", "release", "logs", "codfh.log")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tag")
    ap.add_argument("level")
    ap.add_argument("checkpoint")
    ap.add_argument("x", type=float)
    ap.add_argument("z", type=float)
    ap.add_argument("--vt", default="002E7000")
    ap.add_argument("--limit", type=float, default=600)
    ap.add_argument("--every", type=float, default=1.0)
    ap.add_argument("--then", default="")
    ap.add_argument("--wait", type=float, default=10)
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--mode", default="walk")
    a = ap.parse_args()
    control = os.path.join(ROOT, "work", a.tag + ".control.txt")
    args = [sys.executable, os.path.join(ROOT, "tools", "drive.py"), "--env", "XBE_SKIP_MOVIES=1", "--press",
            "5:START 8:A 12:A 15:A 18:A 62:START 63.5:DOWN 64.5:A 66:DOWN 67:A", "--cmd", "10:level " + a.level,
            "--cmd", "60:" + a.checkpoint, "--cmd", "70:god on", "--until", "72", "--keep", "--tag", a.tag]
    for e in a.env:
        args += ["--env", e]
    subprocess.run(args, capture_output=True)

    def send(line):
        with open(control, "a") as f:
            f.write(line + "\n")

    def actors():
        n0 = sum(1 for _ in open(LOG, encoding="utf-8", errors="replace"))
        send("cmd actors near")
        for _ in range(60):
            time.sleep(0.05)
            lines = open(LOG, encoding="utf-8", errors="replace").read().splitlines()[n0:]
            if any(re.search(r"actors: \d+$", l) for l in lines):
                me, allies = None, []
                for l in lines:
                    m = re.search(r"actors: (\w+) vt (\w+)( \(player\))? at ([-\d.]+) ([-\d.]+) ([-\d.]+)", l)
                    if not m:
                        continue
                    p = (float(m.group(4)), float(m.group(5)), float(m.group(6)))
                    if m.group(3):
                        me = p
                    elif m.group(2) == a.vt:
                        allies.append((m.group(1), p))
                return me, allies
        return None, []

    t0 = time.time()
    best, trail = 1e9, []
    while time.time() - t0 < a.limit:
        me, allies = actors()
        if not me:
            time.sleep(a.every)
            continue
        trail.append(me)
        d = math.hypot(a.x - me[0], a.z - me[2])
        best = min(best, d)
        if d < 600:
            print("arrived", me)
            break
        if allies:
            far = max(allies, key=lambda al: math.hypot(al[1][0] - me[0], al[1][2] - me[2]))
            p = far[1]
            dist = math.hypot(p[0] - me[0], p[2] - me[2])
            if a.mode == "warp":
                if dist > 250 and abs(p[1] - me[1]) < 60:
                    k = max(0.0, (dist - 120) / dist)
                    send("cmd warp %.1f %.1f %.1f" % (me[0] + (p[0] - me[0]) * k, p[1] + 60.5, me[2] + (p[2] - me[2]) * k))
            elif dist > 200:
                # walk to it: the view turned towards it (20 counts a degree, the heading falls turning right), W held
                n0 = sum(1 for _ in open(LOG, encoding="utf-8", errors="replace"))
                send("cmd angles")
                yaw = None
                for _ in range(40):
                    time.sleep(0.05)
                    for l in open(LOG, encoding="utf-8", errors="replace").read().splitlines()[n0:]:
                        m = re.search(r"camera: yaw (\S+)", l)
                        if m:
                            yaw = float(m.group(1))
                    if yaw is not None:
                        break
                if yaw is not None:
                    want = math.degrees(math.atan2(p[0] - me[0], p[2] - me[2]))
                    delta = (want - yaw + 540) % 360 - 180
                    send("cmd look %d 0" % int(-delta * 20))
                    time.sleep(0.12)
                    send("cmd hold 57 %d" % int(min(2500, dist * 4)))
                # stuck for long by the same spot: a warp onto the nearer ally when it stands at the same height
                if len(trail) > 6 and math.hypot(trail[-1][0] - trail[-6][0], trail[-1][2] - trail[-6][2]) < 60:
                    near = min(allies, key=lambda al: math.hypot(al[1][0] - me[0], al[1][2] - me[2]))[1]
                    if abs(near[1] - me[1]) < 60:
                        send("cmd warp %.1f %.1f %.1f" % (near[0], near[1] + 60.5, near[2]))
        time.sleep(a.every)
    print("closest", round(best))
    print("trail", " ".join("%.0f,%.0f,%.0f" % p for p in trail))
    for c in [c for c in a.then.split(";") if c.strip()]:
        send(c.strip())
        time.sleep(0.5)
    time.sleep(a.wait)
    subprocess.run(["taskkill", "/F", "/IM", "CODFH.exe"], capture_output=True)


if __name__ == "__main__":
    main()
