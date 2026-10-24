"""What a build writes to its log in plain play: a copy of the build (out/vk/<copy>) started, a level played for a while
with no test switches, then the log's lines counted per 10 s and by kind (the lines a release writes as it runs: each
one a write, and a report line flushed to the disk).

    python tools/log_rate.py [level] [--build release] [--secs 60] [--copy rate]
"""
import argparse
import os
import re
import subprocess
import sys
import time
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def make(name, build):
    """a copy of build's game in out/vk/<name>: the exe a hard link, the pack a junction, the rest copied"""
    import shutil
    src = os.path.join(ROOT, "out", "build", build)
    d = os.path.join(ROOT, "out", "vk", name)
    os.makedirs(os.path.join(d, "logs"), exist_ok=True)
    exe = os.path.join(d, "CODFH.exe")
    if os.path.exists(exe):
        try:
            os.remove(exe)
        except PermissionError:
            os.replace(exe, os.path.join(d, "CODFH.old%d.exe" % int(time.time() * 1000)))
    os.link(os.path.join(src, "CODFH.exe"), exe)
    release = os.path.join(ROOT, "out", "build", "release")
    if not os.path.exists(os.path.join(d, "remaster")):
        subprocess.run(["cmd", "/c", "mklink", "/J", os.path.join(d, "remaster"), os.path.realpath(os.path.join(release, "remaster"))],
                       capture_output=True)
    for sub in ("lang", "hdd", "shadercache", "tools"):
        s, t = os.path.join(release, sub), os.path.join(d, sub)
        if os.path.isdir(s) and not os.path.exists(t):
            shutil.copytree(s, t)
    toml = open(os.path.join(release, "codfh.toml"), encoding="utf-8").read().splitlines()
    sets = {"display_mode": "'windowed'", "width": "1280", "height": "720", "developer": "false"}
    toml = [("%s = %s" % (l.split("=")[0].strip(), sets[l.split("=")[0].strip()]) if "=" in l and l.split("=")[0].strip() in sets else l)
            for l in toml]
    open(os.path.join(d, "codfh.toml"), "w", encoding="utf-8").write("\n".join(toml) + "\n")
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("level", nargs="?", type=int, default=1)
    ap.add_argument("--build", default="release")
    ap.add_argument("--secs", type=float, default=60)
    ap.add_argument("--copy", default="rate")
    ap.add_argument("--drive", action="store_true")
    a = ap.parse_args()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    if not a.drive:
        d = make(a.copy, a.build)
        env = dict(os.environ, XBE_TOOL_EXE_DIR=d)
        subprocess.run([sys.executable, os.path.abspath(__file__), str(a.level), "--secs", str(a.secs), "--drive"], env=env)
        log = open(os.path.join(d, "logs", "codfh.log"), encoding="utf-8", errors="replace").read().splitlines()
        play = [l for l in log if l.startswith("[")]
        per = Counter()
        kinds = Counter()
        for l in play:
            m = re.match(r"\[\s*([\d.]+)\] (.*)", l)
            if not m:
                continue
            per[int(float(m.group(1)) // 10) * 10] += 1
            kinds[re.sub(r"[0-9a-f]{6,}|\d+(\.\d+)?", "#", m.group(2))[:70]] += 1
        print("lines per 10 s:", sorted(per.items()))
        for k, n in kinds.most_common(40):
            print("%6d  %s" % (n, k))
        return
    import freecam_tour  # noqa: E402
    import drive as dr  # noqa: E402
    game = freecam_tour.Game("rate_%02d" % a.level)
    game.until(10)
    game.send("cmd level %d" % a.level)
    game.until(freecam_tour.PLAY_AT)
    time.sleep(a.secs)
    game.send("quit")
    time.sleep(3)
    dr.kill_build_exe()


if __name__ == "__main__":
    main()
