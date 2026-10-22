"""A level started (movies skipped), the free camera at the player's eye turned by the given yaw and pitch (degrees,
from where the player looks), a screenshot with everything on and one with each effect given off in turn: a sheet at
work/shots/<tag>.png. The case "texview" draws each texture in its own colour (logged: "texview: <id> r g b"), kept
whole at work/shots/<tag>_texview.png; a case "cmd:<command>" sends that command (left in force), "raw:<line>" that
control line.

    python tools/fx_view.py <level> <yaw> <pitch> <effect,effect,...> [--tag name] [--at seconds]
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("level", type=int)
    ap.add_argument("yaw", type=float)
    ap.add_argument("pitch", type=float)
    ap.add_argument("fx")
    ap.add_argument("--tag", default="fxview")
    ap.add_argument("--at", type=float, default=34)
    ap.add_argument("--abs", action="store_true", help="the yaw is the world's, not from where the player looks")
    ap.add_argument("--checkpoint", default="", help='"N s1 s2 s3 s4": restarted there first (freecam_tour places.txt)')
    ap.add_argument("--full", action="store_true", help="each shot also kept whole: <tag>_<case>_full.png")
    ap.add_argument("--pos", default="", help='"x y z": the camera there, not at the eye of the player')
    ap.add_argument("--rel", default="", help='"dx dy dz": the camera that far from the eye of the player (world axes)')
    ap.add_argument("--burst", type=int, default=0, help="then this many shots 0.12 s apart (fast things: splashes)")
    ap.add_argument("--press", default="", help='pad presses before the free camera, as for drive.py: "36:LY-/3"')
    ap.add_argument("--then", default="", help='control lines after the start, ";" between, "sleep N" waits: '
                    '"pad X 150;sleep 8"')
    a = ap.parse_args()
    control = os.path.join(WORK, a.tag + ".control.txt")
    open(control, "w").close()
    replay = os.path.join(WORK, a.tag + ".replay.txt")
    drive.build_replay((START + " " + a.press).strip(), replay)
    with open(replay, "a") as f:
        f.write("%.1f 0 1 0000 0 0 0 0 0 0 0 0 0 0 0 0\n" % 600000)
    drive.kill_build_exe()
    env = dict(os.environ)
    env.update({"XBE_CONTROL": control, "XBE_INPUT_REPLAY": replay, "XBE_SKIP_MOVIES": "1"})
    subprocess.Popen([os.path.join(EXE_DIR, "CODFH.exe")], cwd=EXE_DIR, env=env)
    t0 = time.time()
    # the times count from the shader cache made (a changed shader makes the first start longer)
    time.sleep(3)  # the log begun again
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
    if a.checkpoint:
        while time.time() - t0 < 58:
            time.sleep(0.05)
        send("cmd checkpoint " + a.checkpoint)
        time.sleep(0.5)
        for name, wait in (("START", 1.5), ("DOWN", 1.0), ("A", 1.5), ("DOWN", 1.0), ("A", 0)):
            send("pad %s 150" % name)
            time.sleep(wait)
        for _ in range(34):  # god as soon as the restart takes it: a checkpoint's other sections may drop the player
            send("cmd god on")
            time.sleep(0.5)
    for line in [l.strip() for l in a.then.split(";") if l.strip()]:
        if line.startswith("sleep "):
            time.sleep(float(line[6:]))
        else:
            send(line)
            time.sleep(0.2)
    send("cmd god on")
    send("cmd whereami")
    time.sleep(0.8)
    found = re.findall(r"whereami: (\S+) (\S+) (\S+) yaw (\S+) pitch (\S+)", read_log())
    if not found:
        drive.kill_build_exe()
        sys.exit("no camera")
    x, y, z, yaw0, _ = map(float, found[-1])
    if a.pos:
        x, y, z = map(float, a.pos.split())
    if a.rel:
        dx, dy, dz = map(float, a.rel.split())
        x, y, z = x + dx, y + dy, z + dz
    send("cmd freecam at %.1f %.1f %.1f %.2f %.2f" % (x, y, z, (0 if a.abs else yaw0) + a.yaw, a.pitch))
    time.sleep(1.5)
    cases = ["all"] + [f for f in a.fx.split(",") if f]
    shots = []
    for case in cases:
        if case == "texview":
            send("cmd texview 1")
        elif case.startswith("cmd:"):
            send("cmd " + case[4:])
        elif case.startswith("raw:"):  # a control line as it is ("raw:draws": the frame's draws in the log)
            send(case[4:])
        elif case != "all":
            send("cmd fx %s 0" % case)
        time.sleep(0.6)
        bmp = os.path.join(WORK, "shots", "%s_%d.bmp" % (a.tag, len(shots))).replace("\\", "/")
        if os.path.exists(bmp):
            os.remove(bmp)
        send("shot " + bmp)
        shots.append((case, bmp))
        time.sleep(0.5)
        if case == "texview":
            send("cmd texview 0")
        elif case.startswith("cmd:") or case.startswith("raw:"):
            pass
        elif case != "all":
            send("cmd fx %s 1" % case)
    for i in range(a.burst):
        bmp = os.path.join(WORK, "shots", "%s_b%d.bmp" % (a.tag, i)).replace("\\", "/")
        if os.path.exists(bmp):
            os.remove(bmp)
        send("shot " + bmp)
        shots.append(("b%d" % i, bmp))
        time.sleep(0.12)
    time.sleep(0.5)
    drive.kill_build_exe()
    cols = 3
    rows = (len(shots) + cols - 1) // cols
    sheet = Image.new("RGB", (640 * cols, 360 * rows))
    for i, (case, bmp) in enumerate(shots):
        try:
            im = Image.open(bmp)
            if case == "texview" or a.full:
                im.convert("RGB").save(os.path.join(WORK, "shots", "%s_%s_full.png" % (a.tag, case.replace(":", "_").replace(" ", "_"))))
            im.thumbnail((640, 360))
            ImageDraw.Draw(im).text((8, 8), case, fill=(255, 255, 0))
            sheet.paste(im, ((i % cols) * 640, (i // cols) * 360))
            os.remove(bmp)
        except OSError:
            pass
    out = os.path.join(WORK, "shots", a.tag + ".png")
    sheet.save(out)
    print(out)


if __name__ == "__main__":
    main()
