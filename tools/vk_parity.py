"""The renderer on one graphics API against another: two copies of the game side by side (out/vk/a and out/vk/b, each
with its own log, settings and caches), the same level started in both, the free camera at the player's eye turned
round in both, a shot from each; the pairs side by side with their difference in work/vk/<tag>/lvNN.png.

    python tools/vk_parity.py [levels...] [--a release] [--b vk] [--api-a d3d12] [--api-b vulkan] [--tag parity]
        [--env-a KEY=VALUE,...] [--env-b KEY=VALUE,...]

--a/--b: the build each copy runs (out/build/<name>/CODFH.exe); --api-a/--api-b: XBE_RENDERER for it.
"""
import argparse
import os
import shutil
import subprocess
import sys
import threading
import time

from PIL import Image, ImageChops, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VK = os.path.join(ROOT, "out", "vk")
WORK = os.path.join(ROOT, "work", "vk")
VIEWS = [(45 * k, 0) for k in range(8)] + [(90 * k, 30) for k in range(4)] + [(90 * k + 45, -30) for k in range(4)]


def make(name, build):
    """a copy of build's game in out/vk/<name>: the exe a hard link, the pack a junction, the rest copied"""
    src = os.path.join(ROOT, "out", "build", build)
    d = os.path.join(VK, name)
    os.makedirs(os.path.join(d, "logs"), exist_ok=True)
    exe = os.path.join(d, "CODFH.exe")
    if os.path.exists(exe):
        try:
            os.remove(exe)
        except PermissionError:
            os.replace(exe, os.path.join(d, "CODFH.old%d.exe" % int(time.time() * 1000)))
    os.link(os.path.join(src, "CODFH.exe"), exe)
    # (the build's PDB beside it when it has one: the sampling profiler names functions by it)
    pdb = os.path.join(d, "CODFH.pdb")
    if os.path.exists(pdb):
        os.remove(pdb)
    if os.path.exists(os.path.join(src, "CODFH.pdb")):
        shutil.copy(os.path.join(src, "CODFH.pdb"), pdb)
    dxc = os.path.join(ROOT, "..", "tools", "dxc", "bin", "x64", "dxcompiler.dll")
    if os.path.exists(dxc) and not os.path.exists(os.path.join(d, "dxcompiler.dll")):
        shutil.copy(dxc, d)
    release = os.path.join(ROOT, "out", "build", "release")
    pack = os.path.realpath(os.path.join(release, "remaster"))
    if not os.path.exists(os.path.join(d, "remaster")):
        subprocess.run(["cmd", "/c", "mklink", "/J", os.path.join(d, "remaster"), pack], capture_output=True)
    for sub in ("lang", "hdd", "shadercache", "tools"):
        s, t = os.path.join(release, sub), os.path.join(d, sub)
        if os.path.isdir(s) and not os.path.exists(t):
            shutil.copytree(s, t)
    toml = open(os.path.join(release, "codfh.toml"), encoding="utf-8").read().splitlines()
    sets = {"display_mode": "'windowed'", "width": "1280", "height": "720"}
    toml = [("%s = %s" % (line.split("=")[0].strip(), sets[line.split("=")[0].strip()])
             if "=" in line and line.split("=")[0].strip() in sets else line) for line in toml]
    open(os.path.join(d, "codfh.toml"), "w", encoding="utf-8").write("\n".join(toml) + "\n")
    return d


def run(name, api, levels, tag, extra=""):
    """one copy through the levels: its shots in work/vk/<tag>/<name>/lvNN_vK.bmp (extra: "KEY=VALUE,..." for it alone)"""
    env = dict(os.environ, XBE_TOOL_EXE_DIR=os.path.join(VK, name), XBE_RENDERER=api)
    for kv in filter(None, extra.split(",")):
        k, _, v = kv.partition("=")
        env[k] = v
    return subprocess.Popen([sys.executable, os.path.abspath(__file__), "--drive", name, "--tag", tag] + [str(l) for l in levels],
                            env=env)


def drive(name, levels, tag):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import freecam_tour  # noqa: E402  (after XBE_TOOL_EXE_DIR: the copy's own)
    from level_review import read_log  # noqa: E402
    out = os.path.join(WORK, tag, name)
    os.makedirs(out, exist_ok=True)
    for level in levels:
        game = freecam_tour.Game("vk_%s_%s_%02d" % (tag, name, level))
        game.until(10)
        game.send("cmd level %d" % level)
        game.until(freecam_tour.PLAY_AT)
        game.send("cmd god on")
        place, _ = game.where()
        if not place:
            print(name, "level", level, "no place", flush=True)
            continue
        x, y, z, yaw0 = place
        # VK_PARITY_BEFORE="line;line": control lines sent before the views (tests: "cmd fx heat_haze 0")
        for line in filter(None, os.environ.get("VK_PARITY_BEFORE", "").split(";")):
            game.send(line)
            time.sleep(0.3)
        open(os.path.join(out, "lv%02d_place.txt" % level), "w").write("%f %f %f %f\n" % place)
        want = [int(v) for v in os.environ.get("VK_PARITY_VIEWS", "").split(",") if v]
        for i, (dy, pitch) in enumerate(VIEWS):
            if want and i not in want:
                continue
            if os.environ.get("VK_PARITY_MOVE"):
                # forward and back: the camera along the world's x then z, a step of 300 units a view, looking down a little
                # at the ground (the sun's shadows' cascades change with the distance)
                k = i % 8 - 4
                ox, oz = (k * 300, 0) if i < 8 else (0, k * 300)
                game.send("cmd freecam at %.1f %.1f %.1f %.2f %.2f" % (x + ox, y + 80, z + oz, yaw0 + (90 if i >= 8 else 0), -15))
            else:
                game.send("cmd freecam at %.1f %.1f %.1f %.2f %.2f" % (x, y, z, yaw0 + dy, pitch))
            # VK_PARITY_AT_VIEW="7:line;line": control lines sent with that view (tests)
            at = os.environ.get("VK_PARITY_AT_VIEW", "")
            if at.split(":")[0] == str(i):
                for line in filter(None, at.split(":", 1)[1].split(";")):
                    game.send(line)
            time.sleep(1.5)
            bmp = os.path.join(out, "lv%02d_v%02d.bmp" % (level, i)).replace("\\", "/")
            if os.path.exists(bmp):
                os.remove(bmp)
            game.send("shot " + bmp)
            time.sleep(0.6)
        # VK_PARITY_AFTER="line;line": control lines sent after the views (tests: "draws", "capture")
        for line in filter(None, os.environ.get("VK_PARITY_AFTER", "").split(";")):
            game.send(line)
            time.sleep(2)
        errors = [l for l in read_log().splitlines() if "[E]" in l or "error" in l.lower()]
        open(os.path.join(out, "lv%02d_errors.txt" % level), "w").write("\n".join(errors[-200:]) + "\n")
        game.send("quit")
        time.sleep(3)
        import drive as d  # noqa: E402
        d.kill_build_exe()
        time.sleep(1)


def compare(levels, tag):
    for level in levels:
        rows = []
        for i in range(len(VIEWS)):
            a = os.path.join(WORK, tag, "a", "lv%02d_v%02d.bmp" % (level, i))
            b = os.path.join(WORK, tag, "b", "lv%02d_v%02d.bmp" % (level, i))
            if not (os.path.exists(a) and os.path.exists(b)):
                continue
            ia, ib = Image.open(a).convert("RGB"), Image.open(b).convert("RGB")
            if ia.size != ib.size:
                ib = ib.resize(ia.size)
            diff = ImageChops.difference(ia, ib)
            mean = sum(sum(px) for px in diff.resize((64, 36)).getdata()) / (64 * 36 * 3)
            diff = diff.point(lambda v: min(255, v * 4))
            w, h = 480, 270
            row = Image.new("RGB", (w * 3, h))
            for k, im in enumerate((ia, ib, diff)):
                im = im.copy()
                im.thumbnail((w, h))
                row.paste(im, (k * w, 0))
            ImageDraw.Draw(row).text((6, 4), "view %d  mean diff %.2f" % (i, mean), fill=(255, 255, 0))
            rows.append(row)
        if rows:
            sheet = Image.new("RGB", (rows[0].width, rows[0].height * len(rows)))
            for k, r in enumerate(rows):
                sheet.paste(r, (0, k * r.height))
            sheet.save(os.path.join(WORK, tag, "lv%02d.png" % level))
            print("level", level, "sheet", os.path.join(WORK, tag, "lv%02d.png" % level), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("levels", nargs="*", type=int, default=[1])
    ap.add_argument("--a", default="release")
    ap.add_argument("--b", default="vk")
    ap.add_argument("--api-a", default="d3d12")
    ap.add_argument("--api-b", default="vulkan")
    ap.add_argument("--tag", default="parity")
    ap.add_argument("--env-a", default="", help="environment for copy a alone: KEY=VALUE,...")
    ap.add_argument("--env-b", default="", help="environment for copy b alone")
    ap.add_argument("--drive")
    ap.add_argument("--compare-only", action="store_true")
    ap.add_argument("--views", help="the views to take (indices, comma separated), all by default")
    ap.add_argument("--only", help="one copy alone: a or b")
    a = ap.parse_args()
    if a.drive:
        drive(a.drive, a.levels, a.tag)
        return
    if not a.compare_only:
        if a.views:
            os.environ["VK_PARITY_VIEWS"] = a.views
        procs = []
        if a.only != "b":
            make("a", a.a)
            procs.append(run("a", a.api_a, a.levels, a.tag, a.env_a))
            time.sleep(5)
        if a.only != "a":
            make("b", a.b)
            procs.append(run("b", a.api_b, a.levels, a.tag, a.env_b))
        for p in procs:
            p.wait()
    compare(a.levels, a.tag)


if __name__ == "__main__":
    main()
