"""Tests on the player's own running game (launched with XBE_CONTROL=work/live.control.txt), without firing: the game
unpaused (Escape), each case's effects off in turn with a screenshot, then all back and the game paused again. A case
is effect names joined by '+'; '-' alone is all on. Cases take turns over <rounds> rounds.

    python tools/live_look.py <tag> <rounds> <case> ...
"""
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONTROL = os.path.join(ROOT, "work", "live.control.txt")
SHOTS = os.path.join(ROOT, "work", "shots")


def send(line):
    with open(CONTROL, "a") as f:
        f.write(line + "\n")


def main():
    tag, rounds = sys.argv[1], int(sys.argv[2])
    send("cmd hold 1b 200")  # out of the pause screen
    time.sleep(1.5)
    for r in range(rounds):
        for case in sys.argv[3:]:
            parts = [] if case == "-" else case.split("+")
            for p in parts:
                send("cmd fx %s 0" % p)
            time.sleep(0.4)
            name = "%s_%s_%d" % (tag, "all" if case == "-" else case.replace("+", "-")[:48], r)
            send("shot " + os.path.join(SHOTS, name + ".bmp").replace("\\", "/"))
            time.sleep(0.25)
            for p in parts:
                send("cmd fx %s 1" % p)
            time.sleep(0.3)
    send("cmd hold 1b 200")  # paused again
    time.sleep(0.5)


if __name__ == "__main__":
    main()
