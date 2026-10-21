"""Every model of every level dumped for the remastered meshes (XBE_MESH_DUMP + XBE_MESH_DUMP_ALL): each level
started, then restarted at its checkpoints with the sections about them (freecam_tour's windows), and from each the
free camera flown over a grid of what is loaded, looking down four ways and round at eye height, so the game draws
it all: the level's own models, its objects, vehicles and soldiers. The meshes go to one folder (each once).

    python tools/meshes/dump_sweep.py [levels...] [--out work/meshes/all] [--windows 6] [--radius 24000]
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import drive  # noqa: E402
from freecam_tour import Game, PLAY_AT  # noqa: E402
from level_review import LEVELS, WORK  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def fly(game, center, radius, step, height):
    """the camera over a grid about the center, looking down four ways, then round at eye height at the center"""
    x0, y0, z0 = center[:3]
    k = int(radius // step)
    ring = sorted(((i, j) for i in range(-k, k + 1) for j in range(-k, k + 1)), key=lambda q: abs(q[0]) + abs(q[1]))
    for i, j in ring:
        for yaw in (0, 90, 180, 270):
            game.send("cmd freecam at %.1f %.1f %.1f %d -45" % (x0 + i * step, y0 + height, z0 + j * step, yaw))
            time.sleep(0.3)
    for yaw in range(0, 360, 30):
        game.send("cmd freecam at %.1f %.1f %.1f %d 0" % (x0, y0 + 160, z0, yaw))
        time.sleep(0.3)
    game.send("cmd freecam off")
    time.sleep(0.5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("levels", type=int, nargs="*")
    ap.add_argument("--out", default=os.path.join(ROOT, "work", "meshes", "all"))
    ap.add_argument("--windows", type=int, default=6)
    ap.add_argument("--radius", type=float, default=24000)
    ap.add_argument("--step", type=float, default=6000)
    a = ap.parse_args()
    os.environ["XBE_MESH_DUMP"] = os.path.abspath(a.out)
    os.environ["XBE_MESH_DUMP_ALL"] = "1"
    for level in a.levels or LEVELS:
        before = len(os.listdir(a.out)) if os.path.isdir(a.out) else 0
        game = Game("dumpall_%02d" % level)
        game.until(10)
        game.send("cmd level %d" % level)
        game.until(PLAY_AT)
        game.send("cmd god on")
        place, _ = game.where()
        if place is None:
            print("level %d: no camera" % level, flush=True)
            drive.kill_build_exe()
            continue
        fly(game, place, a.radius, a.step, 3000)
        start_y = place[1]
        for cp in range(1, a.windows + 1):
            sections = (2 * cp - 1, 2 * cp, 2 * cp + 1, 2 * cp + 2)
            game.restart(cp, sections)
            p, falls = game.where()
            if p is None or falls or p[1] < start_y - 2500:
                continue
            fly(game, p, a.radius, a.step, 3000)
        drive.kill_build_exe()
        after = len(os.listdir(a.out))
        print("level %d: %d meshes dumped (%d in all)" % (level, after - before, after), flush=True)


if __name__ == "__main__":
    main()
