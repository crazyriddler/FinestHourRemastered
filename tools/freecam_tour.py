"""Every level's checkpoints looked at from all sides with the free camera, one game a level: the level started and
the player made immune; at each checkpoint (asked for with the sections to load, the level restarted from the pause
screen by pad presses, the sections tried in turn until the player stands rather than falls) the camera at the
player's eye turned round every 45 degrees level, four ways up and four down, then lifted above the player looking
down four ways. A sheet of the twenty shots a place: work/<tag>/lvNN_cpM.png; the places and their sections at
work/<tag>/places.txt (each line also a "checkpoint" control command that starts there). Three checkpoints in a row
with no new place end the level's.

    python tools/freecam_tour.py [levels...] [--checkpoints 1-12] [--tag tour]
"""
import argparse
import math
import os
import re
import subprocess
import threading
import sys
import time

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive  # noqa: E402
from level_review import EXE_DIR, LEVELS, START, WORK, read_log  # noqa: E402

# (yaw from the player's, pitch, up): level round, up, down, from above
VIEWS = ([(45 * k, 0, 0) for k in range(8)] + [(90 * k, 35, 0) for k in range(4)] + [(90 * k + 45, -35, 0) for k in range(4)] +
         [(90 * k, -30, 220) for k in range(4)])
PLAY_AT = 58       # seconds from the start: the level's own opening over
RESTART = 17       # seconds a restart from the pause screen takes to play


class Game:
    def __init__(self, tag):
        self.control = os.path.join(WORK, tag + ".control.txt")
        open(self.control, "w").close()
        # the pad connected all along (a replay of its rest); the menu's presses sent when the game shows its press
        # start screen (anchored there: a copy run beside others boots slower, and presses at fixed times came before
        # the screen and were lost), its clock (until) counted as if that screen came at 4.6 s, as alone
        replay = os.path.join(WORK, tag + ".replay.txt")
        with open(replay, "w") as f:
            f.write("0.0 0 1 0000 0 0 0 0 0 0 0 0 0 0 0 0\n%.1f 0 1 0000 0 0 0 0 0 0 0 0 0 0 0 0\n" % 600000)
        drive.kill_build_exe()
        env = dict(os.environ)
        env.update({"XBE_CONTROL": self.control, "XBE_INPUT_REPLAY": replay, "XBE_SKIP_MOVIES": "1"})
        # XBE_NO_REPLAY: no replay, the keyboard's own (a replay's pad state takes the place of the keys merged into
        # it; the presses here are the control channel's, applied after it either way)
        if os.environ.get("XBE_NO_REPLAY"):
            env.pop("XBE_INPUT_REPLAY", None)
        launched = time.time()
        subprocess.Popen([os.path.join(EXE_DIR, "CODFH.exe")], cwd=EXE_DIR, env=env)
        self.t0 = None
        self.aligned = threading.Event()

        def menu():
            log = os.path.join(EXE_DIR, "logs", "codfh.log")
            while time.time() - launched < 300:
                try:
                    fresh = os.path.getmtime(log) >= launched - 1
                except OSError:
                    fresh = False
                if fresh and "pressStartXbox rewritten" in read_log():
                    break
                time.sleep(0.1)
            self.t0 = time.time() - 4.6
            self.aligned.set()
            for item in START.split():
                at, _, name = item.partition(":")
                while time.time() - self.t0 < float(at):
                    time.sleep(0.02)
                self.send("pad %s 150" % name.split("/")[0])

        threading.Thread(target=menu, daemon=True).start()

    def send(self, line):
        with open(self.control, "a") as f:
            f.write(line + "\n")

    def until(self, t):
        self.aligned.wait()
        while time.time() - self.t0 < t:
            time.sleep(0.05)

    def where(self):
        """the camera's place twice a second apart: (x, y, z, yaw) or None, and whether it falls"""
        n = len(re.findall(r"whereami: ", read_log()))
        self.send("cmd whereami")
        time.sleep(1.0)
        self.send("cmd whereami")
        time.sleep(0.8)
        found = re.findall(r"whereami: (\S+) (\S+) (\S+) yaw (\S+) pitch (\S+)", read_log())
        if len(found) < n + 2:
            return None, False
        a, b = found[-2], found[-1]
        return tuple(map(float, b[:4])), abs(float(a[1]) - float(b[1])) > 60

    def restart(self, cp, sections):
        self.send("cmd checkpoint %d %d %d %d %d" % ((cp,) + tuple(sections)))
        time.sleep(0.5)
        for name, wait in (("START", 1.5), ("DOWN", 1.0), ("A", 1.5), ("DOWN", 1.0), ("A", RESTART)):
            self.send("pad %s 150" % name)
            time.sleep(wait)
        self.send("cmd god on")
        time.sleep(0.3)


def sheet_of(shots, labels, path, cols=4):
    rows = (len(shots) + cols - 1) // cols
    sheet = Image.new("RGB", (480 * cols, 270 * rows))
    for i, bmp in enumerate(shots):
        for _ in range(20):
            if os.path.exists(bmp):
                break
            time.sleep(0.1)
        try:
            im = Image.open(bmp).convert("RGB")
            im.thumbnail((480, 270))
            ImageDraw.Draw(im).text((6, 4), labels[i], fill=(255, 255, 0))
            sheet.paste(im, ((i % cols) * 480, (i // cols) * 270))
            os.remove(bmp)
        except OSError:
            pass
    sheet.save(path)


def shot(game, name, i):
    bmp = os.path.join(WORK, "shots", "%s_%d.bmp" % (name, i)).replace("\\", "/")
    if os.path.exists(bmp):
        os.remove(bmp)
    game.send("shot " + bmp)
    time.sleep(0.4)
    return bmp


def actor_views(game, place, name, out, most=6):
    """the soldiers about (actors): up to six of them spread apart, the view from each one's eye four ways"""
    start = len(read_log())
    game.send("cmd actors")
    time.sleep(1.5)
    log = read_log()[start:]  # this place's
    found = re.findall(r"actors: [0-9A-F]{8} vt [0-9A-F]{8}( \(player\))? at (\S+) (\S+) (\S+),", log)
    pts = [tuple(map(float, f[1:])) for f in found[-400:] if not f[0]]
    pts = [p for p in pts if abs(p[0]) < 1e5 and abs(p[2]) < 1e5 and p[1] > -8000]
    chosen = []
    while pts and len(chosen) < most:
        far = max(pts, key=lambda p: min([math.dist(p, q) for q in chosen + [place[:3]]]))
        if min([math.dist(far, q) for q in chosen + [place[:3]]]) < 300:
            break
        chosen.append(far)
    shots, labels = [], []
    for k, (x, y, z) in enumerate(chosen):
        for yaw in (0, 90, 180, 270):
            game.send("cmd freecam at %.1f %.1f %.1f %d -8" % (x, y + 60, z, yaw))
            time.sleep(0.9)
            shots.append(shot(game, name + "b", len(shots)))
            labels.append("soldier %d at %.0f %.0f %.0f yaw %d" % (k, x, y, z, yaw))
    game.send("cmd freecam off")
    time.sleep(0.8)
    if shots:
        sheet_of(shots, labels, os.path.join(out, name + "_soldiers.png"))
    return len(chosen)


def views(game, place, name, out):
    x, y, z, yaw0 = place
    shots = []
    game.send("cmd freecam at %.1f %.1f %.1f %.2f 0" % (x, y, z, yaw0))
    time.sleep(1.2)
    for i, (dy, pitch, up) in enumerate(VIEWS):
        game.send("cmd freecam at %.1f %.1f %.1f %.2f %.2f" % (x, y + up, z, yaw0 + dy, pitch))
        time.sleep(0.9)
        bmp = os.path.join(WORK, "shots", "%s_%d.bmp" % (name, i)).replace("\\", "/")
        if os.path.exists(bmp):
            os.remove(bmp)
        game.send("shot " + bmp)
        shots.append(bmp)
        time.sleep(0.4)
    game.send("cmd freecam off")
    time.sleep(0.8)
    sheet_of(shots, ["%d: yaw+%d pitch %d%s" % (i, v[0], v[1], " up" if v[2] else "") for i, v in enumerate(VIEWS)],
             os.path.join(out, name + ".png"))
    actor_views(game, place, name, out)


def tour(level, cps, a, out):
    places = os.path.join(out, "places.txt")

    def note(line):
        print(line, flush=True)
        with open(places, "a") as f:
            f.write(line + "\n")

    game = Game("%s_%02d" % (a.tag, level))
    game.until(10)
    game.send("cmd level %d" % level)
    game.until(PLAY_AT)
    game.send("cmd god on")
    place, falls = game.where()
    if place is None:
        note("level %2d: no camera" % level)
        drive.kill_build_exe()
        return
    seen = [place]
    note("level %2d checkpoint 0: at %.1f %.1f %.1f yaw %.1f" % ((level,) + place))
    views(game, place, "lv%02d_cp0" % level, out)
    last_window, misses = 1, 0
    for cp in cps:
        tries = []
        for start in (2 * cp - 1, last_window, last_window + 2, 2 * cp - 3, 2 * cp + 1, last_window + 4, 1):
            if start >= 1 and start not in tries:
                tries.append(start)
        found = None
        for start in tries[:5]:
            sections = [start, start + 1, start + 2, start + 3]
            game.restart(cp, sections)
            place, falls = game.where()
            # (one fallen through to the bottom stands still there, far below)
            if place is not None and not falls and place[1] > -8000:
                found = (sections, place)
                break
        if found is None:
            note("level %2d checkpoint %d: no sections found to stand in" % (level, cp))
            misses += 1
        else:
            sections, place = found
            last_window = sections[0]
            if any(math.dist(p[:3], place[:3]) < 150 for p in seen):
                note("level %2d checkpoint %d: a place seen already (sections %s)" % (level, cp, " ".join(map(str, sections))))
                misses += 1
            else:
                seen.append(place)
                misses = 0
                note("level %2d checkpoint %d: at %.1f %.1f %.1f yaw %.1f, checkpoint %d %s" % ((level, cp) +
                     place + (cp, " ".join(map(str, sections)))))
                views(game, place, "lv%02d_cp%d" % (level, cp), out)
        if misses >= 3:
            break
    with open(os.path.join(out, "lv%02d.log" % level), "w", encoding="utf-8") as f:
        f.write(read_log())
    drive.kill_build_exe()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("levels", type=int, nargs="*")
    ap.add_argument("--checkpoints", default="1-12")
    ap.add_argument("--tag", default="tour")
    a = ap.parse_args()
    lo, _, hi = a.checkpoints.partition("-")
    cps = list(range(int(lo), int(hi or lo) + 1))
    out = os.path.join(WORK, a.tag)
    os.makedirs(out, exist_ok=True)
    for level in a.levels or LEVELS:
        tour(level, cps, a, out)


if __name__ == "__main__":
    main()
