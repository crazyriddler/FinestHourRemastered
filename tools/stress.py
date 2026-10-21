"""Crash hunting: a script of control lines run on the game (movies skipped unless --movies), with the debug heap on
(XBE_POISON: freed memory 0xDD, so a use of it faults where it happens), until the end or a crash. The log is kept in
work/<tag>/stress.log; a crash's guest stack is printed.

Script lines (";" between): "level N" (from the front end), "end N" (the level ended as its script ends it, the next
N started: QuitAndLoadNextLevel), "quit" (to the main menu), "restart K" (checkpoint K with sections 2K-1..2K+2, from
the pause screen), "sleep S", "god", any other control line as it is ("cmd ...", "pad ...").

    python tools/stress.py "level 14; sleep 30; restart 1; sleep 20; end 15; sleep 60; end 17" [--tag stress]
        [--env "XBE_WORLDS_AS_GAME=1"] [--no-poison] [--movies]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive  # noqa: E402
from level_review import EXE_DIR, START, WORK  # noqa: E402

LOG = os.path.join(EXE_DIR, "logs", "codfh.log")


def read_log():
    try:
        return open(LOG, encoding="utf-8", errors="replace").read()
    except OSError:
        return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("script")
    ap.add_argument("--tag", default="stress")
    ap.add_argument("--env", default="")
    ap.add_argument("--no-poison", action="store_true")
    ap.add_argument("--movies", action="store_true")
    a = ap.parse_args()
    out = os.path.join(WORK, a.tag)
    os.makedirs(out, exist_ok=True)
    control = os.path.join(WORK, a.tag + ".control.txt")
    open(control, "w").close()
    replay = os.path.join(WORK, a.tag + ".replay.txt")
    drive.build_replay(START, replay)
    with open(replay, "a") as f:
        f.write("%.1f 0 1 0000 0 0 0 0 0 0 0 0 0 0 0 0\n" % 30000)
    drive.kill_build_exe()
    env = dict(os.environ)
    env.update({"XBE_CONTROL": control, "XBE_INPUT_REPLAY": replay})
    if not a.movies:
        env["XBE_SKIP_MOVIES"] = "1"
    if not a.no_poison:
        env["XBE_POISON"] = "1"
    for kv in filter(None, a.env.split(";")):
        k, v = kv.split("=", 1)
        env[k.strip()] = v.strip()
    subprocess.Popen([os.path.join(EXE_DIR, "CODFH.exe")], cwd=EXE_DIR, env=env)
    t0 = time.time()

    def send(line):
        with open(control, "a") as f:
            f.write(line + "\n")

    def crashed():
        log = read_log()
        return re.search(r"exception C0000|watchdog: no frame", log) is not None

    def wait(s):
        end = time.time() + s
        while time.time() < end:
            if crashed():
                return False
            time.sleep(0.5)
        return True

    while time.time() - t0 < 10:
        time.sleep(0.1)
    ok = True
    for step in [p.strip() for p in a.script.split(";") if p.strip()]:
        w = step.split()
        print("%6.1f %s" % (time.time() - t0, step), flush=True)
        if w[0] == "level":
            send("cmd level %s" % w[1])
            ok = wait(48)
            send("cmd god on")
        elif w[0] == "end":
            send("cmd god on")
            send("cmd endlevel %s" % w[1])
            ok = wait(5)
        elif w[0] == "quit":
            send("cmd endlevel 0")
            ok = wait(5)
        elif w[0] == "restart":
            k = int(w[1])
            send("cmd checkpoint %d %d %d %d %d" % (k, 2 * k - 1, 2 * k, 2 * k + 1, 2 * k + 2))
            time.sleep(0.5)
            for name, pause in (("START", 1.5), ("DOWN", 1.0), ("A", 1.5), ("DOWN", 1.0), ("A", 17)):
                send("pad %s 150" % name)
                time.sleep(pause)
            send("cmd god on")
        elif w[0] == "sleep":
            ok = wait(float(w[1]))
        elif w[0] == "god":
            send("cmd god on")
        else:
            send(step)
            time.sleep(0.3)
        if not ok or crashed():
            ok = False
            break
    time.sleep(2)
    log = read_log()
    shutil.copy(LOG, os.path.join(out, "stress.log"))
    drive.kill_build_exe()
    m = re.search(r"exception C0000.*", log)
    if m or not ok:
        i = log.find("exception C0000")
        if i < 0:
            i = log.find("watchdog: no frame")
        print("CRASH:\n" + log[max(0, i - 600):i + 2500])
    else:
        print("no crash (%.0f s)" % (time.time() - t0))


if __name__ == "__main__":
    main()
