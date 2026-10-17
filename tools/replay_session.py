"""Play a recorded session (XBE_INPUT_RECORD) back from its level's start: the front end is driven to the level
by presses, and when the level starts the recording carries on from its own level start (control::Anchor), the
pad (keyboard included) and the mouse's motion. Screenshots are on the recording's clock (seconds since its start).

    python tools/replay_session.py work/play_rec.txt --level 2 --start 12.598 --shots 40,60 --tag rp

--start: the recording's level start in seconds (its log's "shadows: level N" line). The game is not deterministic
at a free frame rate: the play drifts from the recorded one as it goes.
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import drive  # noqa: E402

MENU = "5:START 8:A 12:A 15:A 18:A"
ANCHOR = 100.0  # the replay's clock at the level's start, past the menu presses


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("recording")
    ap.add_argument("--level", type=int, required=True)
    ap.add_argument("--start", type=float, required=True)
    ap.add_argument("--shots", default="")
    ap.add_argument("--tag", default="rp")
    ap.add_argument("--until", type=float, default=0)
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--line", action="append", default=[])
    a = ap.parse_args()
    out = os.path.join(drive.WORK, a.tag + ".replay.txt")
    drive.build_replay(MENU, out)
    shift = (ANCHOR - a.start) * 1000
    lines = ["%.1f anchor" % (ANCHOR * 1000)]
    end = 0
    with open(a.recording) as f:
        for line in f:
            p = line.split()
            if len(p) < 3:
                continue
            ms = float(p[0])
            if ms < a.start * 1000:
                continue
            if p[1] == "mouse" or (p[1] == "0" and len(p) == 16):
                lines.append("%.1f %s" % (ms + shift, " ".join(p[1:])))
                end = max(end, ms / 1000)
    with open(out, "a") as f:
        f.write("\n".join(lines) + "\n")
    args = [sys.executable, os.path.join(ROOT, "tools", "drive.py"), "--replay", out, "--cmd", "10:level %d" % a.level,
            "--tag", a.tag, "--until", str(a.until or end), "--full", "--env", "XBE_SKIP_MOVIES=1"]
    if a.shots:
        args += ["--shots", a.shots]
    for e in a.env:
        args += ["--env", e]
    for l in a.line:
        args += ["--line", l]
    subprocess.run(args)


if __name__ == "__main__":
    main()
