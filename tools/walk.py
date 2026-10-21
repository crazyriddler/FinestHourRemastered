"""A level restarted at a checkpoint (its sections), the player made immune, then walked by the stick: a list of moves
("LY+ 3000" pushes the left stick up 3 s, "RX+ 400" turns, "sleep 2", "shot", any other control line as it is).
The log is kept in work/<tag>/lvNN.log with its shots; the end says whether the game is still running.

    python tools/walk.py <level> --checkpoint "1 5 6 7 8" --moves "RX+ 800; LY+ 4000; shot; LY- 4000" [--env "A=1"]
        [--tag walk] [--repeat 3]
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
from level_review import EXE_DIR, START, WORK, read_log  # noqa: E402

LOG = os.path.join(EXE_DIR, "logs", "codfh.log")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("level", type=int)
    ap.add_argument("--checkpoint", default="")
    ap.add_argument("--moves", default="")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--env", default="")
    ap.add_argument("--tag", default="walk")
    ap.add_argument("--at", type=float, default=40)
    a = ap.parse_args()
    out = os.path.join(WORK, a.tag)
    os.makedirs(out, exist_ok=True)
    tag = "%s%02d" % (a.tag, a.level)
    control = os.path.join(WORK, tag + ".control.txt")
    open(control, "w").close()
    replay = os.path.join(WORK, tag + ".replay.txt")
    drive.build_replay(START, replay)
    with open(replay, "a") as f:
        f.write("%.1f 0 1 0000 0 0 0 0 0 0 0 0 0 0 0 0\n" % 3600000)
    drive.kill_build_exe()
    env = dict(os.environ)
    env.update({"XBE_CONTROL": control, "XBE_INPUT_REPLAY": replay, "XBE_SKIP_MOVIES": "1"})
    for kv in filter(None, a.env.split(";")):
        k, v = kv.split("=", 1)
        env[k.strip()] = v.strip()
    game = subprocess.Popen([os.path.join(EXE_DIR, "CODFH.exe")], cwd=EXE_DIR, env=env)
    t0 = time.time()

    def send(line):
        with open(control, "a") as f:
            f.write(line + "\n")

    def until(t):
        while time.time() - t0 < t:
            time.sleep(0.05)

    until(10)
    send("cmd level %d" % a.level)
    until(a.at)
    send("cmd god on")
    if a.checkpoint:
        until(max(a.at, 58))
        send("cmd checkpoint " + a.checkpoint)
        time.sleep(0.5)
        for name, wait in (("START", 1.5), ("DOWN", 1.0), ("A", 1.5), ("DOWN", 1.0), ("A", 0)):
            send("pad %s 150" % name)
            time.sleep(wait)
        for _ in range(20):
            send("cmd god on")
            time.sleep(0.5)
    shots = 0
    for _ in range(a.repeat):
        for move in [m.strip() for m in a.moves.split(";") if m.strip()]:
            if game.poll() is not None:
                break
            parts = move.split()
            if parts[0] == "sleep":
                time.sleep(float(parts[1]))
            elif parts[0] == "shot":
                bmp = os.path.join(WORK, "shots", "%s_%d.bmp" % (tag, shots))
                send("shot " + bmp)
                for _ in range(60):
                    time.sleep(0.1)
                    if os.path.exists(bmp):
                        time.sleep(0.2)
                        Image.open(bmp).convert("RGB").save(os.path.join(out, "lv%02d_%d.png" % (a.level, shots)))
                        break
                shots += 1
            elif parts[0][:2] in ("LX", "LY", "RX", "RY") and parts[0][2:] in ("+", "-"):
                ms = int(parts[1]) if len(parts) > 1 else 1000
                send("pad %s %d" % (parts[0], ms))
                time.sleep(ms / 1000 + 0.1)
            else:
                send(move)
                time.sleep(0.2)
    time.sleep(2)
    alive = game.poll() is None
    send("cmd whereami")
    time.sleep(0.8)
    drive.kill_build_exe()
    shutil.copy(LOG, os.path.join(out, "lv%02d.log" % a.level))
    print("game %s; log %s" % ("still running" if alive else "ended (crash?)", os.path.join(out, "lv%02d.log" % a.level)))


if __name__ == "__main__":
    main()
