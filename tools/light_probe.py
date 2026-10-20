"""A level started (movies skipped), the player made immune, then the test light (control "testlight x y z r", a shot's
flash that casts shadows) put at places about the player's eye and a screenshot of each: the dynamic lights' shadows
compared before and after a change (work/<tag>/lvNN_<i>.png, the same places each run).

    python tools/light_probe.py level [--offsets "dx dy dz;..."] [--radius 600] [--tag T] [--then "..."]
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive  # noqa: E402
from hud_probe import EXE_DIR, LOG, START, WORK  # noqa: E402

from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("level", type=int)
    ap.add_argument("--at", type=float, default=40)
    ap.add_argument("--offsets", default="0 0 150;150 0 0;-150 0 0;0 0 -150;0 -60 300;0 -60 -300;300 -60 0;-300 -60 0",
                    help='world offsets from the eye, "dx dy dz" (y up), ";" between')
    ap.add_argument("--radius", type=float, default=600)
    ap.add_argument("--tag", default="lights")
    ap.add_argument("--then", default="", help='control lines after the start, ";" between, "sleep N" waits')
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
    import subprocess
    subprocess.Popen([os.path.join(EXE_DIR, "CODFH.exe")], cwd=EXE_DIR, env=env)
    t0 = time.time()

    def send(line):
        with open(control, "a") as f:
            f.write(line + "\n")

    while time.time() - t0 < 10:
        time.sleep(0.05)
    send("cmd level %d" % a.level)
    while time.time() - t0 < a.at:
        time.sleep(0.05)
    send("cmd god on")
    for part in filter(None, (p.strip() for p in a.then.split(";"))):
        if part.startswith("sleep "):
            time.sleep(float(part[6:]))
        else:
            send(part)
    send("cmd whereami")
    time.sleep(1.0)
    found = re.findall(r"whereami: (\S+) (\S+) (\S+) yaw", open(LOG, encoding="utf-8", errors="replace").read())
    if not found:
        print("no camera")
        drive.kill_build_exe()
        return
    eye = list(map(float, found[-1]))
    for i, off in enumerate(filter(None, (o.strip() for o in a.offsets.split(";")))):
        d = list(map(float, off.split()))
        send("cmd testlight %.1f %.1f %.1f %.0f" % (eye[0] + d[0], eye[1] + d[1], eye[2] + d[2], a.radius))
        time.sleep(0.6)
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
    send("cmd testlight off")
    time.sleep(0.3)
    drive.kill_build_exe()
    print("eye", eye, "shots in", out)


if __name__ == "__main__":
    main()
