"""Catches a stalled game for the record: while CODFH.exe runs, when its log has not grown for --quiet seconds, asks
for every guest thread's stack (control "threads") and keeps the log in work/hangs/. Runs beside a test tool:

    python tools/hang_watch.py <control file> [--quiet 12] [--for 900]
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from level_review import EXE_DIR, WORK  # noqa: E402

LOG = os.path.join(EXE_DIR, "logs", "codfh.log")


def running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq CODFH.exe"], capture_output=True, text=True).stdout
    return "CODFH.exe" in out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("control")
    ap.add_argument("--quiet", type=float, default=12)
    ap.add_argument("--for", dest="total", type=float, default=900)
    a = ap.parse_args()
    t0 = time.time()
    size, since = -1, time.time()
    while time.time() - t0 < a.total:
        time.sleep(1)
        if not running():
            size, since = -1, time.time()
            continue
        s = os.path.getsize(LOG) if os.path.exists(LOG) else 0
        if s != size:
            size, since = s, time.time()
            continue
        if time.time() - since < a.quiet:
            continue
        # a quiet log is no stall while the game still answers (the commands run on its own frames)
        with open(a.control, "a") as f:
            f.write("cmd fps\n")
        time.sleep(3)
        if os.path.getsize(LOG) > size:
            size, since = os.path.getsize(LOG), time.time()
            continue
        with open(a.control, "a") as f:
            f.write("threads\n")
        time.sleep(3)
        os.makedirs(os.path.join(WORK, "hangs"), exist_ok=True)
        keep = os.path.join(WORK, "hangs", time.strftime("hang_%Y%m%d_%H%M%S.log"))
        shutil.copy(LOG, keep)
        grew = os.path.getsize(LOG) > size
        print("stalled %.0f s: %s (threads %s)" % (time.time() - since, keep, "logged" if grew else "not answered"), flush=True)
        return
    print("no stall", flush=True)


if __name__ == "__main__":
    main()
