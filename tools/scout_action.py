"""What happens in each level's first minutes, to choose shots: at a few moments the player's view, the view from
above the player's head (the same heading) and from higher up, and where the actors are (control "actors"). The
player is immune and stands still. work/scout/lvNN.png (rows: the moments; columns: the views), lvNN.txt (actors).

    python tools/scout_action.py [levels...] [--times 40,60,80,100]
"""
import argparse
import os
import re
import subprocess
import sys
import time

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive  # noqa: E402
from level_review import EXE_DIR, START, WORK, read_log  # noqa: E402

LEVELS = [1, 2, 4, 5, 7, 8, 11, 12, 14, 15, 17, 18, 24, 25, 26, 30, 36, 37, 38]
VIEWS = [("player", None), ("above", (150, -8)), ("high", (450, -22)), ("left", (150, -8, -60)), ("right", (150, -8, 60))]


def scout(level, times, out):
    control = os.path.join(WORK, "scout.control.txt")
    open(control, "w").close()
    replay = os.path.join(WORK, "scout.replay.txt")
    drive.build_replay(START, replay)
    with open(replay, "a") as f:
        f.write("%.1f 0 1 0000 0 0 0 0 0 0 0 0 0 0 0 0\n" % 600000)
    drive.kill_build_exe()
    env = dict(os.environ)
    env.update({"XBE_CONTROL": control, "XBE_INPUT_REPLAY": replay, "XBE_SKIP_MOVIES": "1"})
    subprocess.Popen([os.path.join(EXE_DIR, "CODFH.exe")], cwd=EXE_DIR, env=env)
    t0 = time.time()
    time.sleep(3)
    while time.time() - t0 < 600 and "d3d12: shader cache:" not in read_log():
        time.sleep(0.5)
    t0 = time.time() - 1

    def send(line):
        with open(control, "a") as f:
            f.write(line + "\n")

    def until(s):
        while time.time() - t0 < s:
            time.sleep(0.05)

    until(10)
    send("cmd level %d" % level)
    rows, notes = [], []
    for at in times:
        until(at)
        send("cmd god on")
        send("cmd hud 0")
        send("cmd freecam off")
        send("cmd whereami")
        send("cmd actors")
        time.sleep(0.6)
        found = re.findall(r"whereami: (\S+) (\S+) (\S+) yaw (\S+) pitch (\S+)", read_log())
        if not found:
            continue
        x, y, z, yaw, _ = map(float, found[-1])
        log = read_log()
        log = log[log.rfind("control: cmd actors"):]
        log = log[:log.find("actors: ", log.find("away")) if False else len(log)]
        acts = re.findall(r"actors: \S+ vt (\S+)( \(player\))? at (\S+) (\S+) (\S+), (\d+) away", log)
        live = [a for a in acts if not (a[2] == "0.0" and a[3] == "0.0")]
        notes.append("t=%d eye %.0f %.0f %.0f yaw %.1f: %d actors placed %s" % (
            at, x, y, z, yaw, len(live), " ".join("(%s %s %s %sm)" % (a[2], a[3], a[4], int(a[5]) // 100) for a in live[:24])))
        shots = []
        for name, v in VIEWS:
            if v is None:
                send("cmd freecam off")
            else:
                send("cmd freecam at %.1f %.1f %.1f %.1f %.1f" % (x, y + v[0], z, yaw + (v[2] if len(v) > 2 else 0), v[1]))
            time.sleep(0.5)
            bmp = os.path.join(WORK, "shots", "scout_%d_%d_%s.bmp" % (level, at, name)).replace("\\", "/")
            if os.path.exists(bmp):
                os.remove(bmp)
            send("shot " + bmp)
            time.sleep(0.5)
            shots.append((name, bmp))
        send("cmd freecam off")
        rows.append((at, shots))
    time.sleep(0.5)
    drive.kill_build_exe()
    cols = len(VIEWS)
    sheet = Image.new("RGB", (480 * cols, 270 * max(1, len(rows))))
    for r, (at, shots) in enumerate(rows):
        for c, (name, bmp) in enumerate(shots):
            try:
                im = Image.open(bmp).convert("RGB")
                im.thumbnail((480, 270))
                ImageDraw.Draw(im).text((6, 6), "lv%d t%d %s" % (level, at, name), fill=(255, 255, 0))
                sheet.paste(im, (c * 480, r * 270))
                os.remove(bmp)
            except OSError:
                pass
    sheet.save(os.path.join(out, "lv%02d.png" % level))
    with open(os.path.join(out, "lv%02d.txt" % level), "w") as f:
        f.write("\n".join(notes) + "\n")
    print("lv%d: %d moments" % (level, len(rows)), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("levels", nargs="*", type=int)
    ap.add_argument("--times", default="40,60,80,100")
    a = ap.parse_args()
    out = os.path.join(WORK, "scout")
    os.makedirs(out, exist_ok=True)
    for level in a.levels or LEVELS:
        scout(level, [float(t) for t in a.times.split(",")], out)


if __name__ == "__main__":
    main()
