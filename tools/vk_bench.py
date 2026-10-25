"""Direct3D 12 against Vulkan, one after the other on the same copy of the game (out/vk/bench: never both at once,
the GPU all the API's): each level started, the free camera at the player's eye in a few fixed directions, the frame
times recorded there (perf start / perf stop: average, 1% and 0.1% lows, the GPU's time) with V-Sync off and no frame
cap. A table in work/vk/<tag>/bench.md and the raw lines beside it.

    python tools/vk_bench.py [levels...] [--secs 8] [--size 2560x1440] [--apis d3d12,vulkan] [--build vk] [--tag bench]
"""
import argparse
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(ROOT, "work", "vk")
VIEWS = [(0, 0), (90, 0), (180, 0), (270, 0), (45, 25)]


def drive(levels, tag, api, secs, views=None, fxs=()):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import freecam_tour  # noqa: E402
    from level_review import read_log  # noqa: E402
    import drive as d  # noqa: E402
    for level in levels:
        game = freecam_tour.Game("vkb_%s_%s_%02d" % (tag, api, level))
        game.until(10)
        game.send("cmd level %d" % level)
        game.until(freecam_tour.PLAY_AT)
        game.send("cmd god on")
        place, _ = game.where()
        if place:
            x, y, z, yaw0 = place
            for i, (dy, pitch) in enumerate(VIEWS):
                if views and i not in views:
                    continue
                game.send("cmd freecam at %.1f %.1f %.1f %.2f %.2f" % (x, y, z, yaw0 + dy, pitch))
                time.sleep(2.0)  # (the view's textures and pipelines in)
                # the view as it is, then with each of fxs off in turn (what each costs on each API)
                for fx in [None] + list(fxs):
                    if fx:
                        game.send("cmd fx %s 0" % fx)
                        time.sleep(1.5)
                    game.send("cmd perf start")
                    time.sleep(secs)
                    game.send("cmd perf stop %s lv%02d v%d%s" % (api, level, i, " -" + fx if fx else ""))
                    time.sleep(1.5)
                    if fx:
                        game.send("cmd fx %s 1" % fx)
                        time.sleep(0.5)
        # VK_BENCH_AFTER="line;line": control lines sent after the views (tests: "cmd drawtimes")
        for line in filter(None, os.environ.get("VK_BENCH_AFTER", "").split(";")):
            game.send(line)
            time.sleep(2)
        # (each level a new run, its log written over: the reports kept)
        os.makedirs(os.path.join(WORK, tag), exist_ok=True)
        with open(os.path.join(WORK, tag, "raw_%s.txt" % api), "a", encoding="utf-8") as f:
            f.write("".join(l + "\n" for l in read_log().splitlines() if "perf [" in l or "perf: " in l or "drawtime" in l))
        game.send("quit")
        time.sleep(3)
        d.kill_build_exe()
        time.sleep(1)


def parse(log):
    rows = {}
    for m in re.finditer(r"perf \[(\S+) (lv\d+ v\d+(?: -\S+)?)\]: \d+ frames in [\d.]+ s: average ([\d.]+) fps, .*?1% low ([\d.]+) fps, 0\.1% low ([\d.]+) fps,"
                         r" median ([\d.]+) ms", log):
        rows.setdefault(m.group(2), {})[m.group(1)] = {"fps": float(m.group(3)), "low1": float(m.group(4)),
                                                       "low01": float(m.group(5)), "median": float(m.group(6))}
    for m in re.finditer(r"perf \[(\S+) (lv\d+ v\d+(?: -\S+)?)\]: GPU average: ([\d.]+) ms:.*?scene draws ([\d.]+)", log):
        r = rows.setdefault(m.group(2), {}).setdefault(m.group(1), {})
        r["gpu"] = float(m.group(3))
        r["scene"] = float(m.group(4))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("levels", nargs="*", type=int, default=[1, 5, 14, 24])
    ap.add_argument("--secs", type=float, default=8)
    ap.add_argument("--size", default="1920x1080")
    ap.add_argument("--scale", default="133", help="render_percent: 133 of 1080 draws at 1440")
    ap.add_argument("--apis", default="d3d12,vulkan")
    ap.add_argument("--build", default="vk")
    ap.add_argument("--tag", default="bench")
    ap.add_argument("--drive")
    ap.add_argument("--views", help="the views to measure (indices, comma separated), all by default")
    ap.add_argument("--fx", default="", help="effects (fx names) each measured off too, one at a time")
    a = ap.parse_args()
    if a.drive:
        drive(a.levels, a.tag, a.drive, a.secs, [int(v) for v in a.views.split(",")] if a.views else None,
              [f for f in a.fx.split(",") if f])
        return
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import vk_parity  # noqa: E402
    d = vk_parity.make("bench", a.build)
    w, h = a.size.split("x")
    toml = open(os.path.join(d, "codfh.toml"), encoding="utf-8").read().splitlines()
    sets = {"width": w, "height": h, "vsync": "false", "display_mode": "'windowed'", "render_percent": a.scale}
    # VK_BENCH_SET="key=value;key=value": more settings for the run (tests: "anisotropic_filtering=1")
    for kv in filter(None, os.environ.get("VK_BENCH_SET", "").split(";")):
        sets[kv.split("=")[0].strip()] = kv.split("=", 1)[1].strip()
    toml = [("%s = %s" % (l.split("=")[0].strip(), sets[l.split("=")[0].strip()]) if "=" in l and l.split("=")[0].strip() in sets else l)
            for l in toml]
    open(os.path.join(d, "codfh.toml"), "w", encoding="utf-8").write("\n".join(toml) + "\n")
    out = os.path.join(WORK, a.tag)
    os.makedirs(out, exist_ok=True)
    raw = ""
    for api in a.apis.split(","):
        rp = os.path.join(out, "raw_%s.txt" % api)
        if os.path.exists(rp):
            os.remove(rp)
        env = dict(os.environ, XBE_TOOL_EXE_DIR=d, XBE_RENDERER=api, XBE_FPS_CAP="0")
        subprocess.run([sys.executable, os.path.abspath(__file__), "--drive", api, "--tag", a.tag, "--secs", str(a.secs)] +
                       (["--views", a.views] if a.views else []) + (["--fx", a.fx] if a.fx else []) +
                       [str(l) for l in a.levels], env=env)
        if os.path.exists(rp):
            raw += open(rp, encoding="utf-8").read()
    open(os.path.join(out, "raw.txt"), "w", encoding="utf-8").write(raw)
    rows = parse(raw)
    apis = a.apis.split(",")
    lines = ["| view | " + " | ".join("%s fps / 1%% low / GPU ms / scene ms" % x for x in apis) + " |", "|---" * (len(apis) + 1) + "|"]
    sums = {x: [0, 0, 0, 0] for x in apis}
    for key in sorted(rows):
        cells = []
        for x in apis:
            r = rows[key].get(x, {})
            cells.append("%.0f / %.0f / %.2f / %.2f" % (r.get("fps", 0), r.get("low1", 0), r.get("gpu", 0), r.get("scene", 0)))
            if "fps" in r:
                s = sums[x]
                s[0] += r["fps"]
                s[1] += r["low1"]
                s[2] += r.get("gpu", 0)
                s[3] += 1
        lines.append("| %s | %s |" % (key, " | ".join(cells)))
    lines.append("| **average** | " + " | ".join("%.0f / %.0f / %.2f" % (s[0] / max(s[3], 1), s[1] / max(s[3], 1), s[2] / max(s[3], 1))
                                               for s in (sums[x] for x in apis)) + " |")
    open(os.path.join(out, "bench.md"), "w", encoding="utf-8").write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
