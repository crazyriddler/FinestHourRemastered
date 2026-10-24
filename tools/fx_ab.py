"""What an effect shows: in one run, each view shot with the effect on, then off at once (the same moment, the same
clouds and sun), the pair and their difference (x4) in work/fx_ab/<tag>/<fx>_lvNN.png, the mean difference printed.

    python tools/fx_ab.py cloud_shadows 14 15 17 [--pitch -20] [--tag ab] [--build vk]

Runs on a copy of the build in out/vk/ab (never the user's game).
"""
import argparse
import os
import subprocess
import sys
import time

from PIL import Image, ImageChops, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(ROOT, "work", "fx_ab")
YAWS = [0, 60, 120, 180, 240, 300]


def drive(fx, levels, tag, pitch):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import freecam_tour  # noqa: E402
    import drive as d  # noqa: E402
    out = os.path.join(WORK, tag)
    os.makedirs(out, exist_ok=True)
    for level in levels:
        game = freecam_tour.Game("ab_%s_%02d" % (fx, level))
        game.until(10)
        game.send("cmd level %d" % level)
        game.until(freecam_tour.PLAY_AT)
        game.send("cmd god on")
        place, _ = game.where()
        if place:
            x, y, z, yaw0 = place
            for i, dy in enumerate(YAWS):
                game.send("cmd freecam at %.1f %.1f %.1f %.2f %.2f" % (x, y, z, yaw0 + dy, pitch))
                time.sleep(1.5)
                for state in (1, 0):
                    game.send("cmd fx %s %d" % (fx, state))
                    time.sleep(0.4)
                    bmp = os.path.join(out, "%s_lv%02d_v%d_%d.bmp" % (fx, level, i, state)).replace("\\", "/")
                    if os.path.exists(bmp):
                        os.remove(bmp)
                    game.send("shot " + bmp)
                    time.sleep(0.5)
                game.send("cmd fx %s 1" % fx)
        game.send("quit")
        time.sleep(3)
        d.kill_build_exe()
        time.sleep(1)


def sheet(fx, levels, tag):
    out = os.path.join(WORK, tag)
    for level in levels:
        rows, means = [], []
        for i in range(len(YAWS)):
            a = os.path.join(out, "%s_lv%02d_v%d_1.bmp" % (fx, level, i))
            b = os.path.join(out, "%s_lv%02d_v%d_0.bmp" % (fx, level, i))
            if not (os.path.exists(a) and os.path.exists(b)):
                continue
            ia, ib = Image.open(a).convert("RGB"), Image.open(b).convert("RGB")
            diff = ImageChops.difference(ia, ib)
            small = diff.resize((64, 36))
            mean = sum(sum(px) for px in small.get_flattened_data()) / (64 * 36 * 3)
            means.append(mean)
            diff = diff.point(lambda v: min(255, v * 4))
            w, h = 480, 270
            row = Image.new("RGB", (w * 3, h))
            for k, im in enumerate((ia, ib, diff)):
                im = im.copy()
                im.thumbnail((w, h))
                row.paste(im, (k * w, 0))
            ImageDraw.Draw(row).text((6, 4), "view %d  on | off | diff x4  mean %.2f" % (i, mean), fill=(255, 255, 0))
            rows.append(row)
        if rows:
            s = Image.new("RGB", (rows[0].width, rows[0].height * len(rows)))
            for k, r in enumerate(rows):
                s.paste(r, (0, k * r.height))
            path = os.path.join(out, "%s_lv%02d.png" % (fx, level))
            s.save(path)
            print("level %d: mean difference %s -> %s" % (level, " ".join("%.2f" % m for m in means), path), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fx")
    ap.add_argument("levels", nargs="+", type=int)
    ap.add_argument("--pitch", type=float, default=-20)
    ap.add_argument("--tag", default="ab")
    ap.add_argument("--build", default="vk")
    ap.add_argument("--drive", action="store_true")
    a = ap.parse_args()
    if a.drive:
        drive(a.fx, a.levels, a.tag, a.pitch)
        return
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import vk_parity  # noqa: E402
    d = vk_parity.make("ab", a.build)
    env = dict(os.environ, XBE_TOOL_EXE_DIR=d)
    subprocess.run([sys.executable, os.path.abspath(__file__), a.fx] + [str(l) for l in a.levels] +
                   ["--pitch", str(a.pitch), "--tag", a.tag, "--drive"], env=env)
    sheet(a.fx, a.levels, a.tag)


if __name__ == "__main__":
    main()
