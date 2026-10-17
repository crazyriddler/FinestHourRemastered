"""Replay a recorded stretch (XBE_INPUT_RECORD) from a level's checkpoint: the front end driven to the level, the
checkpoint jumped to (control "checkpoint ..." and a restart from the pause screen), then the recording's inputs
from <from> seconds on, shifted to start when the player has control here (at <at> seconds).

    python tools/replay_from.py <recording> <level> "<checkpoint cmd>" <from> --tag t [--at 69] [--until s]
        [--shots a,b] [--cmd "s:cmd"] [--env N=V]
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import drive  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("recording")
    ap.add_argument("level", type=int)
    ap.add_argument("checkpoint")
    ap.add_argument("start", type=float)
    ap.add_argument("--tag", default="rf")
    ap.add_argument("--at", type=float, default=69.0)
    ap.add_argument("--until", type=float, default=0)
    ap.add_argument("--shots", default="")
    ap.add_argument("--cmd", action="append", default=[])
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    out = os.path.join(drive.WORK, a.tag + ".replay.txt")
    drive.build_replay("5:START 8:A 12:A 15:A 18:A 62:START 63.5:DOWN 64.5:A 66:DOWN 67:A", out)
    shift = a.at * 1000 - a.start * 1000
    lines, end = [], 0
    for line in open(a.recording):
        p = line.split()
        if len(p) < 3:
            continue
        ms = float(p[0])
        if ms < a.start * 1000:
            continue
        if p[1] == "mouse" or (p[1] == "0" and len(p) == 16):
            lines.append("%.1f %s" % (ms + shift, " ".join(p[1:])))
            end = max(end, (ms + shift) / 1000)
    # (drive's replay ends with a last line after its presses; the stretch comes after them, in time order)
    with open(out) as f:
        head = [l for l in f.read().splitlines() if l.strip() and float(l.split()[0]) < a.at * 1000]
    with open(out, "w") as f:
        f.write("\n".join(head + lines) + "\n")
    args = [sys.executable, os.path.join(ROOT, "tools", "drive.py"), "--replay", out, "--cmd", "10:level %d" % a.level,
            "--cmd", "60:" + a.checkpoint, "--tag", a.tag, "--until", str(a.until or end + 2), "--full", "--env", "XBE_SKIP_MOVIES=1"]
    if a.shots:
        args += ["--shots", a.shots]
    if a.keep:
        args += ["--keep"]
    for c in a.cmd:
        args += ["--cmd", c]
    for e in a.env:
        args += ["--env", e]
    subprocess.run(args)


if __name__ == "__main__":
    main()
