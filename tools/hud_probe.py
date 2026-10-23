"""The HUD in play: a level started (movies skipped), the player made immune, then screenshots of the player's own
view with the HUD (work/<tag>/lvNN_<i>.png) and, for the first, the frame's draws in the log (their 2D boxes and
where Layout put them). Control lines between shots (--then, ";" between, "sleep N" waits) set things up: "cmd fx
...", a weapon's fire held ("cmd hold 57 500"), ...

    python tools/hud_probe.py level [--shots N] [--then "..."] [--tag T]
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# (XBE_TOOL_EXE_DIR: a copy of the game of its own to run, tools/instances.py, for runs side by side)
EXE_DIR = os.environ.get("XBE_TOOL_EXE_DIR") or os.path.join(ROOT, "out", "build", "release")
LOG = os.path.join(EXE_DIR, "logs", "codfh.log")
WORK = os.path.join(ROOT, "work")
START = "5:START 8:A 12:A 15:A 18:A"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("level", type=int)
    ap.add_argument("--at", type=float, default=34)
    ap.add_argument("--shots", type=int, default=3)
    ap.add_argument("--gap", type=float, default=1.5, help="seconds between shots")
    ap.add_argument("--then", default="")
    ap.add_argument("--tag", default="hud")
    ap.add_argument("--env", default="", help='"NAME=value;..." for the game')
    ap.add_argument("--each", default="", help='control lines before each shot (";" between): "cmd look 60 0"')
    ap.add_argument("--menu", action="store_true", help="no level: the shots from the main menu on (--at its time)")
    ap.add_argument("--mortal", action="store_true", help="the player not made immune (hits: the damage marks)")
    ap.add_argument("--movies", action="store_true", help="the game's movies played (a level's ending film)")
    ap.add_argument("--draws", action="store_true", help="every shot's frame's draws in the log, not the first's alone")
    a = ap.parse_args()
    out = os.path.join(WORK, a.tag)
    os.makedirs(out, exist_ok=True)
    tag = "%s%02d" % (a.tag, a.level)
    control = os.path.join(WORK, tag + ".control.txt")
    open(control, "w").close()
    replay = os.path.join(WORK, tag + ".replay.txt")
    drive.build_replay(START, replay)
    with open(replay, "a") as f:
        f.write("%.1f 0 1 0000 0 0 0 0 0 0 0 0 0 0 0 0\n" % 600000)
    drive.kill_build_exe()
    env = dict(os.environ)
    env.update({"XBE_CONTROL": control, "XBE_INPUT_REPLAY": replay, "XBE_SKIP_MOVIES": "1"})
    if a.movies:
        env.pop("XBE_SKIP_MOVIES")
    for kv in filter(None, a.env.split(";")):
        k, v = kv.split("=", 1)
        env[k.strip()] = v.strip()
    subprocess.Popen([os.path.join(EXE_DIR, "CODFH.exe")], cwd=EXE_DIR, env=env)
    t0 = time.time()

    def send(line):
        with open(control, "a") as f:
            f.write(line + "\n")

    def until(t):
        while time.time() - t0 < t:
            time.sleep(0.02)

    until(10)
    if not a.menu:
        send("cmd level %d" % a.level)
    until(a.at)
    if not a.mortal and not a.menu:
        send("cmd god on")
    for part in filter(None, (p.strip() for p in a.then.split(";"))):
        if part.startswith("sleep "):
            time.sleep(float(part[6:]))
        else:
            send(part)
    time.sleep(1.0)
    for i in range(a.shots):
        for part in filter(None, (p.strip() for p in a.each.split(";"))):
            send(part)
        if a.each:
            time.sleep(0.25)
        if i == 0 or a.draws:
            send("draws")
        bmp = os.path.join(WORK, "shots", "%s_%d.bmp" % (tag, i)).replace("\\", "/")
        if os.path.exists(bmp):
            os.remove(bmp)
        send("shot " + bmp)
        for _ in range(100):
            if os.path.exists(bmp):
                break
            time.sleep(0.05)
        time.sleep(0.3)
        if os.path.exists(bmp):
            Image.open(bmp).convert("RGB").save(os.path.join(out, "lv%02d_%d.png" % (a.level, i)))
        time.sleep(a.gap)
    time.sleep(0.5)
    drive.kill_build_exe()
    shutil.copy(LOG, os.path.join(out, "lv%02d.log" % a.level))
    print("shots in", out)


if __name__ == "__main__":
    main()
