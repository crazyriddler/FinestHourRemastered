"""The weapon in hand on a wide screen, every weapon of each level: the player immune, the HUD hidden, each weapon (the
controller's Y switches) at the hip and aimed (LT held), with the game's own model whole (cmd weaponwhole 1) and as
the port draws it. work/weapons/lvNN.png: rows the weapons and poses, columns whole / as drawn (the screen's left
half, where the arms come in, and the right edge).

    python tools/weapon_check.py [levels...] [--at 40] [--weapons 3]
"""
import argparse
import os
import subprocess
import sys
import time

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive  # noqa: E402
from level_review import EXE_DIR, START, WORK, read_log  # noqa: E402

LEVELS = [1, 2, 4, 5, 7, 8, 11, 12, 14, 15, 17, 18, 24, 25, 26, 30, 36, 37, 38]


def check(level, at, weapons, out):
    control = os.path.join(WORK, "wcheck.control.txt")
    open(control, "w").close()
    replay = os.path.join(WORK, "wcheck.replay.txt")
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
    until(at)
    for _ in range(3):
        send("cmd god on")
        time.sleep(0.3)
    send("cmd hud 0")
    rows = []
    for w in range(weapons):
        for pose in ("hip", "aim"):
            if pose == "aim":
                send("pad LT 2600")
                time.sleep(1.2)
            pair = []
            for whole in (1, 0):
                send("cmd weaponwhole %d" % whole)
                time.sleep(0.25)
                bmp = os.path.join(WORK, "shots", "wc_%d_%d_%s_%d.bmp" % (level, w, pose, whole)).replace("\\", "/")
                if os.path.exists(bmp):
                    os.remove(bmp)
                send("shot " + bmp)
                time.sleep(0.35)
                pair.append(bmp)
            rows.append(("weapon %d %s" % (w, pose), pair))
            time.sleep(1.2)
        send("pad Y 150")
        time.sleep(2.5)
    send("cmd weaponwhole 0")
    time.sleep(0.5)
    drive.kill_build_exe()
    # what the player sees outside the 4:3 view (the side strips, an eighth of the width each, the lower two thirds),
    # at half the full resolution: as drawn, and the game's whole model beside it
    tiles = []
    for label, pair in rows:
        for c, bmp in enumerate(pair):
            try:
                im = Image.open(bmp).convert("RGB")
                os.remove(bmp)
            except OSError:
                continue
            w, h = im.size
            strip = Image.new("RGB", (w // 8 * 2 + 8, h * 2 // 3))
            strip.paste(im.crop((0, h // 3, w // 8, h)), (0, 0))
            strip.paste(im.crop((w - w // 8, h // 3, w, h)), (w // 8 + 8, 0))
            strip = strip.resize((strip.width // 2, strip.height // 2))
            ImageDraw.Draw(strip).text((6, 6), "%s %s" % (label, "whole" if c == 0 else "DRAWN"), fill=(255, 255, 0))
            tiles.append(strip)
    tw, th = tiles[0].size if tiles else (1, 1)
    cols = 4
    sheet = Image.new("RGB", (tw * cols, th * ((len(tiles) + cols - 1) // cols)))
    for i, t in enumerate(tiles):
        sheet.paste(t, ((i % cols) * tw, (i // cols) * th))
    sheet.save(os.path.join(out, "lv%02d.png" % level))
    print("lv%d: %d rows" % (level, len(rows)), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("levels", nargs="*", type=int)
    ap.add_argument("--at", type=float, default=40)
    ap.add_argument("--weapons", type=int, default=3)
    a = ap.parse_args()
    out = os.path.join(WORK, "weapons")
    os.makedirs(out, exist_ok=True)
    for level in a.levels or LEVELS:
        check(level, a.at, a.weapons, out)


if __name__ == "__main__":
    main()
