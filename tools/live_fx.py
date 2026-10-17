"""Tests on the player's own running game (launched with XBE_CONTROL=work/live.control.txt): bursts of fire (the left
button held), each with some effects off and some textures hidden, screenshots in each, then all back. A case is
names joined by '+': an effect (fx) or a texture id (16 hex digits); '-' alone is all on. Each case is played
<rounds> times, the cases taking turns, so each is seen at several moments.

    python tools/live_fx.py <tag> <rounds> <case> ...
"""
import os
import re
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
    send("cmd hold 1 600000")  # firing all along
    time.sleep(1.0)
    for r in range(rounds):
        for case in sys.argv[3:]:
            parts = [] if case == "-" else case.split("+")
            for p in parts:
                send(("cmd hidetex " + p) if re.fullmatch(r"[0-9a-f]{16}", p) else ("cmd fx %s 0" % p))
            time.sleep(0.25)
            name = "%s_%s_%d" % (tag, "all" if case == "-" else case.replace("+", "-")[:48], r)
            send("shot " + os.path.join(SHOTS, name + ".bmp").replace("\\", "/"))
            time.sleep(0.2)
            send("cmd showtex")
            for p in parts:
                if not re.fullmatch(r"[0-9a-f]{16}", p):
                    send("cmd fx %s 1" % p)
            time.sleep(0.25)
            print(name)
    send("cmd hold 1 1")


if __name__ == "__main__":
    main()
