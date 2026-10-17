"""Loads every level in turn through the test level override and reports how each went: a screenshot after the load,
and any crash, error or hang in the log.

    python tools/sweep_levels.py [first] [last] [--at seconds]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "out", "build", "release", "logs", "codfh.log")
DUMPS = os.path.join(ROOT, "out", "build", "release", "CrashDumps")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("first", type=int, nargs="?", default=0)
    ap.add_argument("last", type=int, nargs="?", default=22)
    ap.add_argument("--at", type=float, default=80)
    args = ap.parse_args()
    out = os.path.join(ROOT, "work", "sweep")
    os.makedirs(out, exist_ok=True)
    dumps_before = set(os.listdir(DUMPS)) if os.path.isdir(DUMPS) else set()
    for level in range(args.first, args.last + 1):
        tag = "lv%02d" % level
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "drive.py"), "--press", "5:START 8:A 12:A 15:A 18:A",
                        "--cmd", "10:level %d" % level, "--shots", str(args.at), "--tag", tag,
                        "--until", str(args.at + 1), "--env", "XBE_LOG_VERBOSE=1"], capture_output=True)
        log = open(LOG, encoding="utf-8", errors="replace").read()
        lines = log.splitlines()
        last_t = 0.0
        for l in reversed(lines):
            m = re.match(r"\[\s*([\d.]+)\]", l)
            if m:
                last_t = float(m.group(1))
                break
        # the level's folder, and whether the disc has it (a slot of the game's table without a level stalls)
        folder = next(iter(re.findall(r"LEVELS\\(\w+)\\", log, re.I)), "?")
        missing = "no mapping for LEVELS" in log
        bad = [l for l in lines if re.search(r"crash|exception|fatal|unhandled|assert", l, re.I)][:3]
        shot = os.path.join(ROOT, "work", "shots", "%s_%g.png" % (tag, args.at))
        if os.path.exists(shot):
            shutil.copy2(shot, os.path.join(out, "%s.png" % tag))
        dumps = set(os.listdir(DUMPS)) if os.path.isdir(DUMPS) else set()
        new_dumps = sorted(dumps - dumps_before)
        dumps_before = dumps
        status = "no such level" if missing else "ok" if os.path.exists(shot) and not bad and not new_dumps else "PROBLEM"
        print("level %2d %-10s: %s, log to %.0f s%s%s" % (level, folder, status, last_t, "".join("\n    " + b for b in bad),
                                                   "".join("\n    dump " + d for d in new_dumps)), flush=True)


if __name__ == "__main__":
    main()
