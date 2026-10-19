"""The lighting bake's capture over a whole level: the places a world tour found (tools/world_tour.py's
work/world/lvNN/places.txt: the floors of every window of sections, a place every 14 m), visited again with the free
camera 1.6 m over each, turned round (eight ways level, up and down, half a second each: what it draws is seen in
the 20 frames the capture asks), every window restarted from its checkpoint with its sections as the tour found
them. The game started with XBE_BAKE_CAPTURE=<cod-native>/work/bake/cap (the capture: d3d12.cpp CaptureGeometry).

    python tools/bake/bake_tour.py [levels ...] [--places work/world] [--hold 0.5]
"""
import argparse
import os
import re
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import drive  # noqa: E402
from freecam_tour import Game, PLAY_AT  # noqa: E402

VIEWS = [(45 * k, 0) for k in range(8)] + [(0, 40), (180, -45)]
LEVELS = [1, 2, 4, 5, 7, 8, 11, 12, 14, 15, 17, 18]


def windows_of(path):
    """[(window, sections or None, [places])] from a world tour's places.txt"""
    wins, places = {}, {}
    for line in open(path, encoding="utf-8"):
        m = re.match(r"level\s+\d+ w(\d+)_p\d+: (-?\d+) (-?\d+) (-?\d+)", line)
        if m:
            places.setdefault(int(m.group(1)), []).append(tuple(float(m.group(k)) for k in (2, 3, 4)))
            continue
        m = re.match(r"level\s+\d+ window (\d+) sections (start|[\d ]+): \d+ places", line)
        if m:
            wins[int(m.group(1))] = None if m.group(2) == "start" else tuple(int(x) for x in m.group(2).split())
    return [(w, wins.get(w), places[w]) for w in sorted(places) if w in wins]


def tour(level, a):
    path = os.path.join(a.places, "lv%02d" % level, "places.txt")
    if not os.path.exists(path):
        print("level %d: no places (%s)" % (level, path), flush=True)
        return
    wins = windows_of(path)
    if a.colour:
        # the colours of the baked cells (the reflections' probes): the level's bake loaded, its cells' colours
        # summed (d3d12.cpp BakeColourCapture) into work/bake/col/lvNN.col
        os.environ["XBE_BAKE_COLOR"] = os.path.join(ROOT, "work", "bake", "col")
        os.environ["XBE_BAKE_DIR"] = os.path.join(ROOT, "work", "bake", "out")
        os.environ.pop("XBE_BAKE_CAPTURE", None)
    else:
        cap = os.path.join(ROOT, "work", "bake", "cap")
        os.environ["XBE_BAKE_CAPTURE"] = cap
        # the level's capture anew (the capture appends)
        for ext in ("geo", "eye", "act", "sun"):
            old = os.path.join(cap, "lv%02d.%s" % (level, ext))
            if os.path.exists(old):
                os.remove(old)
    game = Game("bake_%02d" % level)
    game.until(10)
    game.send("cmd level %d" % level)
    game.until(PLAY_AT)
    game.send("cmd god on")
    if a.windows:
        wins = wins[:a.windows]
    for w, sections, places in wins:
        if a.most:
            places = places[:a.most]
        if sections is not None:
            game.restart(w, sections)
            game.send("cmd god on")
            time.sleep(1.0)
        t0 = time.time()
        for x, y, z in places:
            for yaw, pitch in VIEWS:
                game.send("cmd freecam at %.1f %.1f %.1f %d %d" % (x, y + 160, z, yaw, pitch))
                time.sleep(a.hold)
        game.send("cmd freecam off")
        print("level %d window %d: %d places in %.0f s" % (level, w, len(places), time.time() - t0), flush=True)
    # (the colours are written every 10 s)
    time.sleep(12 if a.colour else 1)
    drive.kill_build_exe()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("levels", nargs="*", type=int)
    ap.add_argument("--places", default=os.path.join(ROOT, "work", "world"))
    ap.add_argument("--hold", type=float, default=0.5)
    ap.add_argument("--most", type=int, default=0, help="places a window at most (tests)")
    ap.add_argument("--windows", type=int, default=0, help="windows at most (tests)")
    ap.add_argument("--colour", action="store_true", help="the baked cells' colours (work/bake/col), not the geometry")
    a = ap.parse_args()
    for level in a.levels or LEVELS:
        tour(level, a)


if __name__ == "__main__":
    main()
