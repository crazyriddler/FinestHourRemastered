"""Tests on the player's own running game (XBE_CONTROL=work/live.control.txt): bursts of fire with one texture's
draws hidden in each (hidetex), a screenshot in each.

    python tools/live_hide.py <tag> <texture id | -> ...
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
    tag = sys.argv[1]
    for tex in sys.argv[2:]:
        if tex != "-":
            for t in tex.split(","):
                send("cmd hidetex " + t)
        time.sleep(0.2)
        send("cmd hold 1 2000")
        time.sleep(1.3)
        name = "%s_%s" % (tag, tex.replace(",", "+").replace("-", "none")[:40])
        for k in range(2):
            send("shot " + os.path.join(SHOTS, "%s_%d.bmp" % (name, k)).replace("\\", "/"))
            time.sleep(0.3)
        send("cmd showtex")
        time.sleep(0.7)
        print(name)


if __name__ == "__main__":
    main()
