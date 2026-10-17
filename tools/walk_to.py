"""Walk the player to a place (tests): the level started at a checkpoint (drive.py, kept running), then a loop on the
control file: the player's place and heading read back (whereami, angles), the view turned towards the target
(look: 20 counts a degree, positive turns right, the heading falling) and W held. Stuck (hardly moved): a turn to
the right (or left, alternately more) and a few steps, then the target again. Ends near the target or at the time
limit; the game is left running for the caller's commands (the control file, the log), then closed.

    python tools/walk_to.py <tag> <level> <checkpoint cmd> <x> <z> [--limit 400] [--then "cmd;cmd"] [--wait 20]
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
sys.path.insert(0, os.path.join(ROOT, "tools"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tag")
    ap.add_argument("level")
    ap.add_argument("checkpoint")
    ap.add_argument("x", type=float)
    ap.add_argument("z", type=float)
    ap.add_argument("--limit", type=float, default=400)
    ap.add_argument("--then", default="")
    ap.add_argument("--wait", type=float, default=20)
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--mode", default="wall")
    a = ap.parse_args()
    control = os.path.join(ROOT, "work", a.tag + ".control.txt")
    args = [sys.executable, os.path.join(ROOT, "tools", "drive.py"), "--env", "XBE_SKIP_MOVIES=1", "--press",
            "5:START 8:A 12:A 15:A 18:A 62:START 63.5:DOWN 64.5:A 66:DOWN 67:A", "--cmd", "10:level " + a.level,
            "--cmd", "60:" + a.checkpoint, "--until", "72", "--keep", "--tag", a.tag]
    for e in a.env:
        args += ["--env", e]
    subprocess.run(args, capture_output=True)

    def send(line):
        with open(control, "a") as f:
            f.write(line + "\n")

    def ask():
        n0 = sum(1 for _ in open(LOG, encoding="utf-8", errors="replace"))
        send("cmd whereami")
        send("cmd angles")
        for _ in range(40):
            time.sleep(0.05)
            lines = open(LOG, encoding="utf-8", errors="replace").read().splitlines()[n0:]
            pos = yaw = None
            for l in lines:
                m = re.search(r"whereami: (\S+) (\S+) (\S+)", l)
                if m:
                    pos = tuple(map(float, m.groups()))
                m = re.search(r"camera: yaw (\S+)", l)
                if m:
                    yaw = float(m.group(1))
            if pos and yaw is not None:
                return pos, yaw
        return None, None

    t0 = time.time()
    last = None
    trail = []
    free = 0  # steps moved freely since the last turn
    best = 1e9
    while time.time() - t0 < a.limit:
        pos, yaw = ask()
        if not pos:
            continue
        trail.append(pos)
        d = math.hypot(a.x - pos[0], a.z - pos[2])
        best = min(best, d)
        if d < 250:
            print("arrived", pos)
            break
        moved = math.hypot(pos[0] - last[0], pos[2] - last[2]) if last else 999
        last = pos
        if a.mode == "greedy":
            if moved < 25:
                send("cmd look %d 0" % int(60 * 20))
                time.sleep(0.15)
                send("cmd hold 57 900")
                time.sleep(1.0)
                continue
            want = math.degrees(math.atan2(a.x - pos[0], a.z - pos[2]))
            delta = (want - yaw + 540) % 360 - 180
            send("cmd look %d 0" % int(-delta * 20 * 0.7))
        else:
            # the right hand on the wall: blocked, turn left; moving on for a while, a turn right to find the wall
            if moved < 20:
                send("cmd look %d 0" % int(-40 * 20))
                free = 0
            else:
                free += 1
                if free >= 3:
                    send("cmd look %d 0" % int(35 * 20))
                    free = 0
        time.sleep(0.12)
        send("cmd hold 57 650")
        time.sleep(0.6)
    print("closest", round(best))
    print("trail", " ".join("%.0f,%.0f,%.0f" % p for p in trail[::5]))
    for c in [c for c in a.then.split(";") if c.strip()]:
        send(c.strip())
        time.sleep(0.5)
    time.sleep(a.wait)
    subprocess.run(["taskkill", "/F", "/IM", "CODFH.exe"], capture_output=True)


if __name__ == "__main__":
    main()
