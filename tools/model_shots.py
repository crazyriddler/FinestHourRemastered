"""The remastered models against the game's own, in game: a level started (movies skipped), each weapon given in turn
and pictured in hand with Remastered Models on and off (the same frame held: the game paused by the screenshot's
own wait is not needed, the weapon still at rest), kept whole at work/shots/<tag>_<weapon>_{on,off}.png.

    python tools/model_shots.py <level> <weapon id,...> [--tag models] [--pitch -10] [--at 40]

Weapon ids: player_tests.cpp (7 MP40, 8 PPSh, 10 Thompson, 14 Mosin Nagant, 15 M1 Garand, 16 Kar98, ...); "held" the
one the level starts with.
"""
import argparse
import os
import subprocess
import sys
import time

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive  # noqa: E402
from level_review import EXE_DIR, START, WORK, read_log  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("level", type=int)
    ap.add_argument("weapons")
    ap.add_argument("--tag", default="models")
    ap.add_argument("--at", type=float, default=40)
    ap.add_argument("--then", default="", help='control lines before the shots, ";" between, "sleep N" waits')
    a = ap.parse_args()
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
    time.sleep(3)
    t0 = time.time()
    while time.time() - t0 < 600 and "d3d12: shader cache:" not in read_log():
        time.sleep(0.5)
    t0 = time.time() - 1

    def send(line):
        with open(control, "a") as f:
            f.write(line + "\n")

    while time.time() - t0 < 10:
        time.sleep(0.05)
    send("cmd level %d" % a.level)
    while time.time() - t0 < a.at:
        time.sleep(0.05)
    send("cmd god on")
    for line in [l.strip() for l in a.then.split(";") if l.strip()]:
        if line.startswith("sleep "):
            time.sleep(float(line[6:]))
        else:
            send(line)
            time.sleep(0.2)
    os.makedirs(os.path.join(WORK, "shots"), exist_ok=True)
    for w in a.weapons.split(","):
        if w != "held":  # ("held": the weapon the level starts with)
            send("cmd giveweapon %s" % w)
        time.sleep(6)  # streamed, drawn and at rest
        for case, on in (("on", 1), ("off", 0)):
            send("cmd fx remastered_models %d" % on)
            time.sleep(0.6)
            bmp = os.path.join(WORK, "shots", "%s_%s_%s.bmp" % (a.tag, w, case)).replace("\\", "/")
            if os.path.exists(bmp):
                os.remove(bmp)
            send("shot " + bmp)
            time.sleep(0.8)
        send("cmd fx remastered_models 1")
    time.sleep(1)
    drive.kill_build_exe()
    for w in a.weapons.split(","):
        for case in ("on", "off"):
            bmp = os.path.join(WORK, "shots", "%s_%s_%s.bmp" % (a.tag, w, case))
            try:
                Image.open(bmp).convert("RGB").save(bmp[:-4] + ".png")
                os.remove(bmp)
                print(bmp[:-4] + ".png")
            except OSError:
                print("missing", bmp)


if __name__ == "__main__":
    main()
