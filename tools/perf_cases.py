"""The frame's GPU time under a few settings: a level started (movies skipped), the view held still, each case's control
lines sent in turn and the frame measured ("perf [...]: GPU average" lines), the round twice (the scene changes as it
plays: the two rounds' average); each case's steps that differ most from the first case's listed.

    python tools/perf_cases.py <level> "<case>" ["<case>" ...] [--at 60] [--secs 4] [--then "line;line"]

A case is control lines (";" between), e.g. "cmd fx aa 0" "cmd fx aa 3"; --then: lines after the start (a free camera).
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
    ap.add_argument("cases", nargs="+")
    ap.add_argument("--at", type=float, default=60)
    ap.add_argument("--secs", type=float, default=4)
    ap.add_argument("--then", default="")
    a = ap.parse_args()
    control = os.path.join(WORK, "perf_cases.control.txt")
    open(control, "w").close()
    replay = os.path.join(WORK, "perf_cases.replay.txt")
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
    send("cmd level %d" % a.level)
    until(a.at)
    send("cmd god on")
    for line in filter(None, (l.strip() for l in a.then.split(";"))):
        send(line)
    t = a.at + 1.5
    for rnd in range(2):
        for k, case in enumerate(a.cases):
            for line in filter(None, (l.strip() for l in case.split(";"))):
                send(line)
            until(t + 1.5)
            send("cmd perf start")
            until(t + 1.5 + a.secs)
            send("cmd perf stop case%d r%d" % (k, rnd))
            t += a.secs + 1.7
    until(t + 1)
    drive.kill_build_exe()
    log = read_log()
    res = {}
    for m in re.finditer(r"perf \[case(\d+) r(\d)\]: GPU average: ([\d.]+) ms:(.*)", log):
        steps = {}
        for part in m.group(4).split(","):
            mm = re.match(r"(.+) ([\d.]+)$", part.strip())
            if mm:
                steps[mm.group(1)] = float(mm.group(2))
        res.setdefault(int(m.group(1)), []).append((float(m.group(3)), steps))
    base = None
    for k, case in enumerate(a.cases):
        if k not in res:
            print("%-40s (not measured)" % case)
            continue
        ms = sum(r[0] for r in res[k]) / len(res[k])
        steps = {}
        for _, st in res[k]:
            for n, v in st.items():
                steps[n] = steps.get(n, 0) + v / len(res[k])
        if base is None:
            base = steps
        diffs = sorted(((steps.get(n, 0) - base.get(n, 0), n) for n in set(base) | set(steps)), key=lambda x: -abs(x[0]))
        top = ", ".join("%s %+.2f" % (n, d) for d, n in diffs[:4] if abs(d) >= 0.03) if k else ""
        spread = max(r[0] for r in res[k]) - min(r[0] for r in res[k])
        print("%-40s %5.2f ms (rounds %.2f apart)   %s" % (case, ms, spread, top), flush=True)


if __name__ == "__main__":
    main()
