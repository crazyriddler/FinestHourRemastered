"""A release folder tried as a player would start it: its exe from its own folder (the settings, pack and shaders it
holds), the main menu, a level, a picture of each, then quit; the log is checked for the shader cache's start (every
pipeline prepared before play), the game found, and errors.

    python tools/release/test_install.py "<install folder>" [--level 1] [--tag port]
"""
import argparse
import os
import re
import subprocess
import sys
import time

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORK = os.path.join(ROOT, "work")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import drive  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--level", type=int, default=1)
    ap.add_argument("--tag", default="port")
    a = ap.parse_args()
    exe = os.path.join(a.folder, "CODFH.exe")
    log = os.path.join(a.folder, "logs", "codfh.log")
    control = os.path.join(WORK, a.tag + ".control.txt")
    open(control, "w").close()
    env = dict(os.environ)
    env["XBE_CONTROL"] = control
    # the front end's presses as the review tools give them (START, the profile, a new game), from the start
    replay = os.path.join(WORK, a.tag + ".replay.txt")
    drive.build_replay("5:START 8:A 12:A 15:A 18:A", replay)
    with open(replay, "a") as f:
        f.write("%.1f 0 1 0000 0 0 0 0 0 0 0 0 0 0 0 0\n" % 600000)
    env["XBE_INPUT_REPLAY"] = replay
    if os.path.exists(log):
        os.remove(log)
    t0 = time.time()
    subprocess.Popen([exe], cwd=a.folder, env=env)

    def send(line):
        with open(control, "a") as f:
            f.write(line + "\n")

    def text():
        try:
            return open(log, encoding="utf-8", errors="replace").read()
        except OSError:
            return ""

    while time.time() - t0 < 900 and "d3d12: shader cache:" not in text():
        time.sleep(0.5)
    ready = time.time() - t0
    shots = []
    # the level asked for on the way to a new game (the replay's presses)
    steps = [(9.5, "shot menu"), (10, "cmd level %d" % a.level), (75, "shot level")]
    for at, what in ((s, w) for s, w in steps):
        while time.time() - t0 < at:
            time.sleep(0.1)
        if what.startswith("shot"):
            bmp = os.path.join(WORK, "shots", "%s_%s.bmp" % (a.tag, what[5:])).replace("\\", "/")
            send("shot " + bmp)
            shots.append(bmp)
        else:
            send(what)
    time.sleep(2)
    send("quit")
    time.sleep(3)
    for bmp in shots:
        if os.path.exists(bmp):
            Image.open(bmp).convert("RGB").save(bmp[:-4] + ".png")
            os.remove(bmp)
    t = text()
    print("shaders ready %.1f s after the start" % ready)
    for pat in (r"Path: .*", r"d3d12: shader cache: .*", r"offline: level \d+ .*", r"shadows: level \d+", r"watchdog: .*",
                r"exception .*", r"\[E\].*", r"error.*"):
        for m in re.findall(pat, t)[:3]:
            print("  " + m[:200])


if __name__ == "__main__":
    main()
