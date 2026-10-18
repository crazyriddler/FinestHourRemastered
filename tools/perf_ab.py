"""GPU and frame cost of effects, A/B at one fixed view: a level started (movies skipped), the free camera at the
player's eye turned by the yaw and pitch given (from where the player looks; --abs: the world's), then for each case
the frame times recorded a few seconds (perf start/stop) with that case's effects off, everything on between cases
and twice (first and last) as a reference. Prints each case's frame and GPU averages and its GPU steps.

    python tools/perf_ab.py <level> <yaw> <pitch> "fx1+fx2,fx3,..." [--secs 4] [--abs] [--tag name]
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("level", type=int)
    ap.add_argument("yaw", type=float)
    ap.add_argument("pitch", type=float)
    ap.add_argument("cases")
    ap.add_argument("--secs", type=float, default=4)
    ap.add_argument("--abs", action="store_true")
    ap.add_argument("--at", type=float, default=34)
    ap.add_argument("--tag", default="perfab")
    ap.add_argument("--paired", action="store_true",
                    help="everything on measured again just before each case: the case's cost against it (a scene that "
                         "changes as it plays drifts less between the two)")
    a = ap.parse_args()
    control = os.path.join(WORK, a.tag + ".control.txt")
    open(control, "w").close()
    replay = os.path.join(WORK, a.tag + ".replay.txt")
    drive.build_replay(START, replay)
    with open(replay, "a") as f:
        f.write("%.1f 0 1 0000 0 0 0 0 0 0 0 0 0 0 0 0\n" % 600000)
    drive.kill_build_exe()
    env = dict(os.environ)
    env.update({"XBE_CONTROL": control, "XBE_INPUT_REPLAY": replay, "XBE_SKIP_MOVIES": "1"})
    subprocess.Popen([os.path.join(EXE_DIR, "CODFH.exe")], cwd=EXE_DIR, env=env)
    t0 = time.time()

    def send(line):
        with open(control, "a") as f:
            f.write(line + "\n")

    while time.time() - t0 < 10:
        time.sleep(0.05)
    send("cmd level %d" % a.level)
    while time.time() - t0 < a.at:
        time.sleep(0.05)
    send("cmd god on")
    send("cmd whereami")
    time.sleep(0.8)
    found = re.findall(r"whereami: (\S+) (\S+) (\S+) yaw (\S+) pitch (\S+)", read_log())
    if not found:
        drive.kill_build_exe()
        sys.exit("no camera")
    x, y, z, yaw0, _ = map(float, found[-1])
    send("cmd freecam at %.1f %.1f %.1f %.2f %.2f" % (x, y, z, (0 if a.abs else yaw0) + a.yaw, a.pitch))
    time.sleep(2.0)
    cases = ["all"] + [c for c in a.cases.split(",") if c] + ["all again"]
    if a.paired:
        cases = ["all"] + [x for c in a.cases.split(",") if c for x in ("all before " + c, c)] + ["all again"]

    def turn(f, off):
        # "name=value": the effect set to that value, 1 again after (shadows=2: the shadows' quality)
        name, _, value = f.partition("=")
        send("cmd fx %s %s" % (name, (value or "0") if off else "1"))

    for i, case in enumerate(cases):
        fx = [] if case.startswith("all") else case.split("+")
        for f in fx:
            turn(f, True)
        time.sleep(0.6)
        send("cmd perf start")
        time.sleep(a.secs)
        send("cmd perf stop C%d" % i)
        time.sleep(0.8)
        for f in fx:
            turn(f, False)
        time.sleep(0.3)
    drive.kill_build_exe()
    log = read_log()
    before = None
    for i, case in enumerate(cases):
        fr = re.findall(r"perf \[C%d\]: \d+ frames in [\d.]+ s: average ([\d.]+) fps.*median ([\d.]+) ms" % i, log)
        gpu = re.findall(r"perf \[C%d\]: GPU average: ([\d.]+) ms:(.*)" % i, log)
        steps = gpu[-1][1].strip()[:240] if gpu else ""
        if a.paired and case.startswith("all before"):
            before = float(gpu[-1][0]) if gpu else None
            continue
        delta = ""
        if a.paired and before is not None and gpu and not case.startswith("all"):
            delta = " saves %+.2f ms" % (before - float(gpu[-1][0]))
        print("%-28s %s fps (median %s ms), GPU %s ms%s | %s" % (case, fr[-1][0] if fr else "?", fr[-1][1] if fr else "?",
                                                                gpu[-1][0] if gpu else "?", delta, steps), flush=True)


if __name__ == "__main__":
    main()
