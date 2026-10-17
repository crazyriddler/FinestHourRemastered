"""Every level walked through with the free camera, as much of it as the game loads: the level started and the player
made immune; then for each window of sections (the start's, then each checkpoint restart with sections 1-4, 3-6,
5-8 ... until two windows add little new) the floors drawn are mapped (the control channel's "map": the free camera
turned round about the player and above), places spread over the new floors (a point each 14 m at most, the nearest
first), and from each, 1.6 m over its floor, the camera turned round every 45 degrees level, four ways up, four
down. Each shot is checked for the defects seen before (tools/defects.py: lines across the sky, sparkles, black
holes, magenta) and the place's sheet written with the suspects framed in red: work/<tag>/lvNN/wK_pM.png, the
suspects listed in work/<tag>/lvNN/suspects.txt, the places in places.txt.

    python tools/world_tour.py [levels...] [--tag world] [--per-window 40] [--spacing 1400] [--windows 8] [--most 140]
"""
import argparse
import math
import os
import re
import sys
import time

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import defects  # noqa: E402
import drive  # noqa: E402
from freecam_tour import Game, PLAY_AT  # noqa: E402
from level_review import EXE_DIR, LEVELS, WORK, read_log  # noqa: E402

MAP = os.path.join(EXE_DIR, "logs", "map.txt")
VIEWS = [(45 * k, 0) for k in range(8)] + [(90 * k + 20, 35) for k in range(4)] + [(90 * k + 65, -40) for k in range(4)]


def read_map():
    cells = []
    try:
        for line in open(MAP):
            v = list(map(int, line.split()))
            if len(v) >= 3:
                cells += [(v[0], y, v[1]) for y in v[2:]]
    except OSError:
        pass
    return cells


def gather_map(game, place):
    """the floors seen from the place turned round, level and down, and from 8 m over it"""
    x, y, z = place[:3]
    if os.path.exists(MAP):
        os.remove(MAP)
    n = len(re.findall(r"map: \d+ cells written", read_log()))
    game.send("cmd map 100000")
    for up, pitches in ((0, (0, -35)), (800, (-45,))):
        for pitch in pitches:
            for yaw in range(0, 360, 45):
                game.send("cmd freecam at %.1f %.1f %.1f %d %d" % (x, y + up, z, yaw, pitch))
                time.sleep(0.35)
    game.send("cmd map 1")
    for _ in range(60):
        time.sleep(0.25)
        if len(re.findall(r"map: \d+ cells written", read_log())) > n:
            break
    game.send("cmd freecam off")
    time.sleep(0.5)
    return read_map()


def walkable(cells, start, step=170):
    """the floor cells reached from the start on foot: neighbouring cells (3 m) no more than a step up or down"""
    by = {}
    for c in cells:
        by.setdefault((c[0] // 300, c[2] // 300), []).append(c)
    first = min(cells, key=lambda c: math.dist(c, start), default=None)
    if first is None or math.dist(first, start) > 600:
        return []
    got, todo = {first}, [first]
    while todo:
        c = todo.pop()
        kx, kz = c[0] // 300, c[2] // 300
        for dx in (-1, 0, 1):
            for dz in (-1, 0, 1):
                for n in by.get((kx + dx, kz + dz), []):
                    if n not in got and abs(n[1] - c[1]) <= step:
                        got.add(n)
                        todo.append(n)
    return list(got)


def survey(game, center, radius, step=4500, height=3500):
    """the floors of what is loaded, seen from above: the camera over a grid about the center, looking down four ways"""
    x0, y0, z0 = center[:3]
    if os.path.exists(MAP):
        os.remove(MAP)
    n = len(re.findall(r"map: \d+ cells written", read_log()))
    game.send("cmd map 100000")
    k = int(radius // step)
    ring = sorted(((i, j) for i in range(-k, k + 1) for j in range(-k, k + 1)), key=lambda q: abs(q[0]) + abs(q[1]))
    for i, j in ring:
        for yaw in (0, 90, 180, 270):
            game.send("cmd freecam at %.1f %.1f %.1f %d -55" % (x0 + i * step, y0 + height, z0 + j * step, yaw))
            time.sleep(0.25)
    game.send("cmd map 1")
    for _ in range(60):
        time.sleep(0.25)
        if len(re.findall(r"map: \d+ cells written", read_log())) > n:
            break
    game.send("cmd freecam off")
    time.sleep(0.5)
    return read_map()


def open_floors(cells):
    """floors that are areas (a ground, a yard, a roof), not a wall's top or a beam: of the eight neighbouring 3 m
    cells, three or more have a floor within a metre of its height"""
    by = {}
    for c in cells:
        by.setdefault((c[0] // 300, c[2] // 300), []).append(c[1])
    got = []
    for c in cells:
        kx, kz = c[0] // 300, c[2] // 300
        n = sum(1 for dx in (-1, 0, 1) for dz in (-1, 0, 1) if (dx or dz) and
                any(abs(y - c[1]) < 100 for y in by.get((kx + dx, kz + dz), [])))
        if n >= 3:
            got.append(c)
    return got


def spread(cells, start, seen, spacing, most):
    """places over the floors, each at least spacing from the others and from those seen before, the nearest first"""
    pts = sorted(cells, key=lambda c: math.dist(c, start))
    chosen = []
    for c in pts:
        if all(math.dist(c, q) >= spacing for q in chosen + seen):
            chosen.append(c)
            if len(chosen) >= most:
                break
    return chosen


def shoot(game, name):
    bmp = os.path.join(WORK, "shots", name + ".bmp").replace("\\", "/")
    if os.path.exists(bmp):
        os.remove(bmp)
    game.send("shot " + bmp)
    return bmp


def views(game, at, name, out, suspects):
    """the shots about a place; the floors seen meanwhile mapped (returned)"""
    x, y, z = at
    if os.path.exists(MAP):
        os.remove(MAP)
    nmap = len(re.findall(r"map: \d+ cells written", read_log()))
    game.send("cmd map 100000")
    game.send("cmd god on")
    bmps = []
    for i, (yaw, pitch) in enumerate(VIEWS):
        game.send("cmd freecam at %.1f %.1f %.1f %d %d" % (x, y + 160, z, yaw, pitch))
        time.sleep(0.75 if i else 1.2)
        bmps.append(shoot(game, "%s_%d" % (name, i)))
        time.sleep(0.35)
    game.send("cmd map 1")
    for _ in range(40):
        time.sleep(0.25)
        if len(re.findall(r"map: \d+ cells written", read_log())) > nmap:
            break
    cells = read_map()
    tiles = []
    for i, bmp in enumerate(bmps):
        im = None
        for _ in range(30):
            if os.path.exists(bmp):
                try:
                    im = Image.open(bmp).convert("RGB")
                    break
                except OSError:
                    pass
            time.sleep(0.1)
        if im is None:
            tiles.append((None, []))
            continue
        found = defects.check(im)
        if found:
            # the shot whole kept for a closer look
            im.save(os.path.join(out, "%s_v%d.png" % (name, i)))
            suspects.append("%s view %d (yaw %d pitch %d) at %.0f %.0f %.0f: %s" %
                            (name, i, VIEWS[i][0], VIEWS[i][1], x, y + 160, z, "; ".join(found)))
        tiles.append((im, found))
        try:
            os.remove(bmp)
        except OSError:
            pass
    cols, tw, th = 4, 640, 360
    sheet = Image.new("RGB", (cols * tw, 4 * th))
    for i, (im, found) in enumerate(tiles):
        if im is None:
            continue
        t = im.resize((tw, th), Image.BILINEAR)
        d = ImageDraw.Draw(t)
        d.text((6, 4), "%d yaw %d pitch %d" % (i, VIEWS[i][0], VIEWS[i][1]), fill=(255, 255, 0))
        if found:
            d.rectangle((0, 0, tw - 1, th - 1), outline=(255, 0, 0), width=4)
            d.text((6, 18), " / ".join(found)[:90], fill=(255, 80, 80))
        sheet.paste(t, ((i % cols) * tw, (i // cols) * th))
    sheet.save(os.path.join(out, name + ".png"))
    return cells


def tour(level, a, root):
    out = os.path.join(root, "lv%02d" % level)
    os.makedirs(out, exist_ok=True)
    places = os.path.join(out, "places.txt")
    suspects = []

    def note(line):
        print(line, flush=True)
        with open(places, "a") as f:
            f.write(line + "\n")

    game = Game("%s_%02d" % (a.tag, level))
    game.until(10)
    game.send("cmd level %d" % level)
    game.until(PLAY_AT)
    game.send("cmd god on")
    place, _ = game.where()
    if place is None:
        note("level %2d: no camera" % level)
        drive.kill_build_exe()
        return
    seen_pts, total, start_y = [], 0, place[1]
    # the start, then each checkpoint with the sections about it (freecam_tour's first guess: 2k-1 to 2k+2, then a
    # window either side)
    windows = [None] + [(k, (2 * k - 1, 2 * k, 2 * k + 1, 2 * k + 2)) for k in range(1, a.windows + 1)]
    dry = 0
    for wi, win in enumerate(windows):
        if total >= a.most:
            break
        sections = None
        if win is not None:
            cp, first = win
            p, falls = None, True
            for sections in (first, tuple(x - 2 for x in first), tuple(x + 2 for x in first)):
                if sections[0] < 1:
                    continue
                game.restart(cp, sections)
                game.send("cmd god on")
                p, falls = game.where()
                if p is not None and not falls and p[1] > start_y - 2500:
                    break
            # (fallen through the world, or still falling: these sections are not where the player stands)
            if p is None or falls or p[1] < start_y - 2500:
                note("level %2d window %d (checkpoint %d): the player falls, left out" % (level, wi, win[0]))
                dry += 1
                if dry >= 3:
                    break
                continue
            place = p
        # the places: from where the player stands, then from each place the floors its shots showed, the nearest to
        # the last one first, none nearer than spacing to one seen
        # what this window loaded, from above: its floors not near a place seen are where to go
        known = set(survey(game, place, a.radius))
        fresh = [c for c in open_floors(list(known)) if all(math.dist(c, q) >= a.spacing for q in seen_pts)]
        note("level %2d window %d: %d floor cells loaded, %d fresh" % (level, wi, len(known), len(fresh)))
        if len(fresh) < 8:
            dry += 1
            if dry >= 3:
                break
            continue
        todo, at, done = [place[:3]] + spread(fresh, place[:3], seen_pts, a.spacing, 400), place[:3], 0
        while todo and done < a.per_window and total < a.most:
            todo.sort(key=lambda c: math.dist(c, at))
            pt = todo.pop(0)
            if any(math.dist(pt, q) < a.spacing * 0.6 for q in seen_pts):
                continue
            seen_pts.append(pt)
            name = "w%d_p%02d" % (wi, done)
            note("level %2d %s: %.0f %.0f %.0f" % (level, name, pt[0], pt[1], pt[2]))
            cells = views(game, pt, name, out, suspects)
            done += 1
            total += 1
            at = pt
            known.update(cells)
            for c in spread(open_floors(list(known)), pt, seen_pts + todo, a.spacing, 60):
                todo.append(c)
            with open(os.path.join(out, "suspects.txt"), "w") as f:
                f.write("\n".join(suspects) + "\n")
        note("level %2d window %d sections %s: %d places" % (level, wi, "start" if sections is None else " ".join(map(str, sections)), done))
        dry = dry + 1 if done <= 1 else 0
        if dry >= 3:
            break
    with open(os.path.join(out, "game.log"), "w", encoding="utf-8") as f:
        f.write(read_log())
    drive.kill_build_exe()
    note("level %2d: %d places, %d suspect shots" % (level, len(seen_pts), len(suspects)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("levels", type=int, nargs="*")
    ap.add_argument("--tag", default="world")
    ap.add_argument("--per-window", type=int, default=40)
    ap.add_argument("--spacing", type=float, default=1100)
    ap.add_argument("--windows", type=int, default=10)
    ap.add_argument("--radius", type=float, default=27000, help="the survey's reach about the player")
    ap.add_argument("--most", type=int, default=140, help="places a level at most")
    a = ap.parse_args()
    root = os.path.join(WORK, a.tag)
    os.makedirs(root, exist_ok=True)
    for level in a.levels or LEVELS:
        tour(level, a, root)


if __name__ == "__main__":
    main()
