"""Each graphics preset's GPU time: a level started (movies skipped), the view held still, every preset applied in turn
(Very High to Original), its frame's GPU time measured ("perf [...]: GPU average" lines), the round twice (the scene
changes as it plays: the two rounds' average), the steps that differ most from Very High listed.

    python tools/perf_presets.py <level> [level ...] [--at 60] [--secs 4] [--cam "x y z yaw pitch" ...]

--cam: a free camera held there (one per level, in order): the same view for every preset. Measure with the frame
rate uncapped and V-Sync off (codfh.toml fps_cap = 0, vsync = false): at a cap the GPU idles and lowers its clocks.
"""
import argparse
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive  # noqa: E402
from level_review import EXE_DIR, START, WORK, read_log  # noqa: E402

NAMES = ["Very High", "High", "Medium", "Low", "Original"]


def run(level, a, cam):
    control = os.path.join(WORK, "perf_presets.control.txt")
    open(control, "w").close()
    replay = os.path.join(WORK, "perf_presets.replay.txt")
    drive.build_replay(START, replay)
    drive.kill_build_exe()
    env = dict(os.environ)
    env.update({"XBE_CONTROL": control, "XBE_INPUT_REPLAY": replay, "XBE_SKIP_MOVIES": "1"})
    subprocess.Popen([os.path.join(EXE_DIR, "CODFH.exe")], cwd=EXE_DIR, env=env)
    t0 = time.time()

    def send(line):
        with open(control, "a") as f:
            f.write(line + "\n")

    time.sleep(3)
    while time.time() - t0 < 600 and "d3d12: shader cache:" not in read_log():
        time.sleep(0.5)
    t0 = time.time() - 1

    def until(t):
        while time.time() - t0 < t:
            time.sleep(0.05)

    until(10)
    send("cmd level %d" % level)
    until(a.at)
    send("cmd god on")
    if cam:
        send("cmd freecam at " + cam)
    t = a.at + 1
    for rnd in range(2):
        for p in range(5):
            send("cmd fx preset %d" % (p + 1))
            until(t + 1.5)  # (settled: pipelines, the bake's lookups)
            send("cmd perf start")
            until(t + 1.5 + a.secs)
            send("cmd perf stop preset%d r%d" % (p, rnd))
            t += a.secs + 1.7
    send("cmd fx preset 1")
    until(t + 1)
    drive.kill_build_exe()
    log = read_log()
    res = {}
    for m in re.finditer(r"perf \[preset(\d) r(\d)\]: GPU average: ([\d.]+) ms:(.*)", log):
        steps = {}
        for part in m.group(4).split(","):
            mm = re.match(r"(.+) ([\d.]+)$", part.strip())
            if mm:
                steps[mm.group(1)] = float(mm.group(2))
        res.setdefault(int(m.group(1)), []).append((float(m.group(3)), steps))
    print("level %d" % level)
    base = None
    for p in range(5):
        if p not in res:
            continue
        ms = sum(r[0] for r in res[p]) / len(res[p])
        steps = {}
        for _, st in res[p]:
            for k, v in st.items():
                steps[k] = steps.get(k, 0) + v / len(res[p])
        if base is None:
            base = steps
        diffs = sorted(((base.get(k, 0) - steps.get(k, 0), k) for k in set(base) | set(steps)), reverse=True)
        top = ", ".join("%s %.2f" % (k, d) for d, k in diffs[:5] if d >= 0.03) if p else ""
        spread = max(r[0] for r in res[p]) - min(r[0] for r in res[p])
        print("  %-10s %5.2f ms (rounds %.2f apart)   %s" % (NAMES[p], ms, spread, top), flush=True)
    # the steps by preset (the costliest on Very High first)
    table = {}
    for p in res:
        for _, st in res[p]:
            for k, v in st.items():
                table.setdefault(k, [0.0] * 5)[p] += v / len(res[p])
    for k in sorted(table, key=lambda k: -table[k][0])[:16]:
        print("    %-22s %s" % (k, " ".join("%5.2f" % v for v in table[k])))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("levels", nargs="+", type=int)
    ap.add_argument("--at", type=float, default=60)
    ap.add_argument("--secs", type=float, default=4)
    ap.add_argument("--cam", action="append", default=[])
    a = ap.parse_args()
    for k, level in enumerate(a.levels):
        run(level, a, a.cam[k] if k < len(a.cam) else "")


if __name__ == "__main__":
    main()
