"""Each level reviewed: started (movies skipped) and the player made immune; the free camera at the player's eye
looking all the way round and up and down (a screenshot every 45 degrees); then, with every frame's time recorded
(perf start/stop), the free camera turned smoothly round twice, up and down, round fast, the player's own view turned
with the mouse and the player walking. work/review/lvNN.png (the screenshots), work/review/summary.txt (the frame
rates: average, min, max, 1% and 0.1% lows, the worst frames and what they did).

    python tools/level_review.py [levels...] [--at seconds]
"""
import argparse
import os
import re
import subprocess
import sys
import time

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE_DIR = os.path.join(ROOT, "out", "build", "release")
LOG = os.path.join(EXE_DIR, "logs", "codfh.log")
WORK = os.path.join(ROOT, "work")
LEVELS = [1, 2, 4, 5, 7, 8, 11, 12, 14, 15, 17, 18, 24, 25, 26, 30, 36, 37, 38]  # the 19 missions (the game's level ids)
START = "5:START 8:A 12:A 15:A 18:A"


def read_log():
    try:
        return open(LOG, encoding="utf-8", errors="replace").read()
    except OSError:
        return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("levels", type=int, nargs="*")
    ap.add_argument("--at", type=float, default=34, help="seconds from the start to the level's play")
    ap.add_argument("--tag", default="review")
    a = ap.parse_args()
    levels = a.levels or LEVELS
    out = os.path.join(WORK, a.tag)
    os.makedirs(out, exist_ok=True)
    summary = []
    for level in levels:
        tag = "%s%02d" % (a.tag, level)
        control = os.path.join(WORK, tag + ".control.txt")
        open(control, "w").close()
        replay = os.path.join(WORK, tag + ".replay.txt")
        drive.build_replay(START, replay)
        with open(replay, "a") as f:
            f.write("%.1f 0 1 0000 0 0 0 0 0 0 0 0 0 0 0 0\n" % 600000)
        drive.kill_build_exe()
        env = dict(os.environ)
        env.update({"XBE_CONTROL": control, "XBE_INPUT_REPLAY": replay, "XBE_SKIP_MOVIES": "1"})
        subprocess.Popen([os.path.join(EXE_DIR, "CODFH.exe")], cwd=EXE_DIR, env=env)
        t0 = time.time()

        def send(line):
            with open(control, "a") as f:
                f.write(line + "\n")

        def until(t):
            while time.time() - t0 < t:
                time.sleep(0.02)

        until(10)
        send("cmd level %d" % level)
        until(a.at)
        send("cmd god on")
        send("cmd whereami")
        time.sleep(0.8)
        found = re.findall(r"whereami: (\S+) (\S+) (\S+) yaw (\S+) pitch (\S+)", read_log())
        if not found:
            line = "level %2d: no camera (did it start?)" % level
            print(line, flush=True)
            summary.append(line)
            drive.kill_build_exe()
            continue
        x, y, z, yaw0, pitch0 = map(float, found[-1])
        here = "%.1f %.1f %.1f" % (x, y, z)

        cur = [yaw0, 0.0]

        def turn(yaw, pitch, seconds, steps_per_s=30):
            steps = max(1, int(seconds * steps_per_s))
            y0, p0 = cur
            for i in range(1, steps + 1):
                f = i / steps
                send("cmd freecam at %s %.2f %.2f" % (here, y0 + (yaw - y0) * f, p0 + (pitch - p0) * f))
                time.sleep(seconds / steps)
            cur[0], cur[1] = yaw, pitch

        # the screenshots: round every 45 degrees, then up and down
        shots = []
        send("cmd freecam at %s %.2f 0" % (here, yaw0))
        time.sleep(1.0)
        views = [(yaw0 + 45 * k, 0) for k in range(8)] + [(yaw0, 40), (yaw0, -40)]
        for i, (yw, pt) in enumerate(views):
            turn(yw, pt, 0.5)
            time.sleep(0.6)
            bmp = os.path.join(WORK, "shots", "%s_%d.bmp" % (tag, i)).replace("\\", "/")
            if os.path.exists(bmp):
                os.remove(bmp)
            send("shot " + bmp)
            shots.append(bmp)
            time.sleep(0.4)
        # the frame times
        turn(yaw0, 0, 0.5)
        time.sleep(0.5)
        send("cmd perf start")
        time.sleep(0.3)
        turn(yaw0 + 360, 0, 6)       # round, smoothly
        turn(yaw0 + 720, 0, 6)       # and again
        turn(yaw0 + 720, 45, 1.5)    # up
        turn(yaw0 + 720, -45, 3)     # down
        turn(yaw0 + 720, 0, 1.5)
        turn(yaw0 + 1080, 0, 2)      # round fast
        send("cmd freecam off")
        time.sleep(0.5)
        for dx in (40, -40):         # the player's own view, by the mouse
            for _ in range(60):
                send("cmd look %d 0" % dx)
                time.sleep(0.05)
        send("cmd hold 57 4000")     # walking (W)
        time.sleep(4.5)
        send("cmd perf stop L%02d" % level)
        time.sleep(1.5)
        drive.kill_build_exe()

        log = read_log()
        with open(os.path.join(out, "lv%02d.log" % level), "w", encoding="utf-8") as f:
            f.write(log)
        perf = [l.split("]: ", 1)[1] for l in log.splitlines() if "perf [L%02d]" % level in l]
        cache = re.findall(r"shader cache: (.*)", log)
        errors = [l for l in log.splitlines() if re.search(r"error|failed|exception", l, re.I)][:5]
        compiles = [l for l in log.splitlines() if "compile failed" in l][:3]
        line = "level %2d:\n  %s\n  cache: %s%s%s" % (
            level, "\n  ".join(perf) or "no frame times", cache[-1] if cache else "?",
            "".join("\n  ERROR " + e for e in errors), "".join("\n  " + c for c in compiles))
        print(line, flush=True)
        summary.append(line)
        sheet = Image.new("RGB", (480 * 5, 270 * 2))
        for i, bmp in enumerate(shots):
            try:
                im = Image.open(bmp)
                im.thumbnail((480, 270))
                sheet.paste(im, ((i % 5) * 480, (i // 5) * 270))
                os.remove(bmp)
            except OSError:
                pass
        sheet.save(os.path.join(out, "lv%02d.png" % level))
        with open(os.path.join(out, "summary.txt"), "w") as f:
            f.write("\n".join(summary) + "\n")


if __name__ == "__main__":
    main()
