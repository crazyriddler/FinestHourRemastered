"""What each effect costs the GPU: a level started (movies skipped), the view held still, the frame's GPU steps measured
with everything on, then with each effect off in turn (back on after), then everything on again; the difference each
makes, by step, from the log's "perf [...]: GPU average" lines.

    python tools/perf_fx.py <level> [--at 60] [--secs 6] [--env NAME=VALUE ...] [effects ...]

Effects are the control's "fx" names (game_info.cpp Effect), e.g. ao contact_shadows bounce_light; "shadows" is the
shadows' quality (off: 0), "aa" the anti-aliasing (off: 0, back to 4x MSAA); none: a list of the costly ones.
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

DEFAULT = ["ao", "contact_shadows", "bounce_light", "reflections", "volumetric_fog", "dynamic_lights", "light_halos",
           "ambient_particles", "sun_shafts", "clouds", "sun_moon", "soft_shadows", "materials", "parallax", "detail",
           "pixel_lighting", "smooth_models", "aa", "bloom", "water", "lit_smoke", "heat_haze", "baked_lighting",
           "motion_blur", "auto_exposure", "color_grade", "film_grain", "cloud_shadows", "character_light"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("level", type=int)
    ap.add_argument("effects", nargs="*")
    ap.add_argument("--at", type=float, default=60)
    ap.add_argument("--secs", type=float, default=3)
    ap.add_argument("--env", action="append", default=[])
    a = ap.parse_args()
    effects = a.effects or DEFAULT
    control = os.path.join(WORK, "perf_fx.control.txt")
    open(control, "w").close()
    replay = os.path.join(WORK, "perf_fx.replay.txt")
    drive.build_replay(START, replay)
    drive.kill_build_exe()
    env = dict(os.environ)
    env.update({"XBE_CONTROL": control, "XBE_INPUT_REPLAY": replay, "XBE_SKIP_MOVIES": "1"})
    for e in a.env:
        k, _, v = e.partition("=")
        env[k] = v
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
    send("cmd level %d" % a.level)
    until(a.at)
    send("cmd god on")
    t = a.at + 1
    # each effect: on, off, on again (the scene changes as it plays: the two on around the off cancel its drift)
    def measure(label):
        nonlocal t
        until(t + 0.6)  # (settled: pipelines, history)
        send("cmd perf start")
        until(t + 0.6 + a.secs)
        send("cmd perf stop %s" % label)
        t += a.secs + 0.8

    for fx in effects:
        measure("%s on1" % fx)
        send("cmd fx %s 0" % fx)
        measure("%s off" % fx)
        send("cmd fx %s %s" % (fx, "3" if fx in ("shadows", "aa") else "1"))
        measure("%s on2" % fx)
    until(t + 1)
    drive.kill_build_exe()
    log = read_log()
    res = {}
    for m in re.finditer(r"perf \[([^\]]*)\]: GPU average: ([\d.]+) ms:(.*)", log):
        steps = {}
        for part in m.group(3).split(","):
            part = part.strip()
            mm = re.match(r"(.+) ([\d.]+)$", part)
            if mm:
                steps[mm.group(1)] = float(mm.group(2))
        res[m.group(1)] = (float(m.group(2)), steps)
    rows = []
    for fx in effects:
        k1, k0, k2 = "%s on1" % fx, "%s off" % fx, "%s on2" % fx
        if not all(k in res for k in (k1, k0, k2)):
            continue
        on_total = (res[k1][0] + res[k2][0]) / 2
        names = set(res[k1][1]) | set(res[k0][1]) | set(res[k2][1])
        diffs = []
        for n in names:
            on = (res[k1][1].get(n, 0) + res[k2][1].get(n, 0)) / 2
            diffs.append((on - res[k0][1].get(n, 0), n))
        diffs.sort(reverse=True)
        top = ", ".join("%s %.2f" % (n, d) for d, n in diffs[:3] if d >= 0.02)
        drift = abs(res[k1][0] - res[k2][0])
        rows.append((on_total - res[k0][0], fx, top, drift))
    for saved, fx, top, drift in sorted(rows, reverse=True):
        print("%-20s %5.2f ms   (drift %.2f)   %s" % (fx, saved, drift, top))


if __name__ == "__main__":
    main()
