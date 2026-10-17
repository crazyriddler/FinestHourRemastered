"""Steer the running game's player (tests): along waypoints (x,z pairs) then to the target, the view turned towards
the next point (look: 20 counts a degree, the heading falls turning right) and W held; stuck, turns right and left
by turns; a waypoint is passed within 250. Reads the player's place from the log (whereami), the heading (angles).

    python tools/steer.py <tag> <x> <z> [--points "x,z x,z ..."] [--limit 300] [--then "cmd a;cmd b"] [--wait 5]
"""
import argparse
import math
import os
import re
import subprocess
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "out", "build", "release", "logs", "codfh.log")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tag")
    ap.add_argument("x", type=float)
    ap.add_argument("z", type=float)
    ap.add_argument("--points", default="")
    ap.add_argument("--limit", type=float, default=300)
    ap.add_argument("--then", default="")
    ap.add_argument("--wait", type=float, default=5)
    ap.add_argument("--close", action="store_true")
    a = ap.parse_args()
    control = os.path.join(ROOT, "work", a.tag + ".control.txt")
    pts = [tuple(map(float, p.split(","))) for p in a.points.split()] + [(a.x, a.z)]

    def send(line):
        with open(control, "a") as f:
            f.write(line + "\n")

    def ask():
        n0 = sum(1 for _ in open(LOG, encoding="utf-8", errors="replace"))
        send("cmd whereami")
        send("cmd angles")
        for _ in range(40):
            time.sleep(0.05)
            pos = yaw = None
            for l in open(LOG, encoding="utf-8", errors="replace").read().splitlines()[n0:]:
                m = re.search(r"whereami: (\S+) (\S+) (\S+)", l)
                if m:
                    pos = tuple(map(float, m.groups()))
                m = re.search(r"camera: yaw (\S+)", l)
                if m:
                    yaw = float(m.group(1))
            if pos and yaw is not None:
                return pos, yaw
        return None, None

    # start at the waypoint nearest ahead
    pos, _ = ask()
    i = 0
    if pos:
        i = min(range(len(pts)), key=lambda k: math.hypot(pts[k][0] - pos[0], pts[k][1] - pos[2]))
    t0, last, turn, trail = time.time(), None, 50.0, []
    while time.time() - t0 < a.limit:
        pos, yaw = ask()
        if not pos:
            continue
        trail.append(pos)
        tx, tz = pts[i]
        if math.hypot(tx - pos[0], tz - pos[2]) < 250:
            if i == len(pts) - 1:
                print("arrived", pos)
                break
            i += 1
            continue
        moved = math.hypot(pos[0] - last[0], pos[2] - last[2]) if last else 999
        last = pos
        if moved < 25:
            send("cmd look %d 0" % int(turn * 20))
            turn = -turn * 1.3 if abs(turn) < 120 else 50.0
            time.sleep(0.12)
            send("cmd hold 57 900")
            time.sleep(1.0)
            continue
        want = math.degrees(math.atan2(tx - pos[0], tz - pos[2]))
        delta = (want - yaw + 540) % 360 - 180
        send("cmd look %d 0" % int(-delta * 20 * 0.8))
        time.sleep(0.12)
        send("cmd hold 57 700")
        time.sleep(0.6)
    print("waypoint", i, "of", len(pts))
    print("trail", " ".join("%.0f,%.0f,%.0f" % p for p in trail[::4]))
    for c in [c for c in a.then.split(";") if c.strip()]:
        send(c.strip())
        time.sleep(0.5)
    time.sleep(a.wait)
    if a.close:
        subprocess.run(["taskkill", "/F", "/IM", "CODFH.exe"], capture_output=True)


if __name__ == "__main__":
    main()
