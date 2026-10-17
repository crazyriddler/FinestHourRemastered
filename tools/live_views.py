"""In a running game (its control file): the free camera put at given views, and at each a screenshot with all on
and one with each effect given off.

    python tools/live_views.py <control tag> <out tag> "x y z yaw pitch" ["x y z yaw pitch" ...] [--fx a,b,c]
"""
import argparse
import os
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHOTS = os.path.join(ROOT, "work", "shots")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ctag")
    ap.add_argument("tag")
    ap.add_argument("views", nargs="+")
    ap.add_argument("--fx", default="")
    a = ap.parse_args()
    control = os.path.join(ROOT, "work", a.ctag + ".control.txt")

    def send(line):
        with open(control, "a") as f:
            f.write(line + "\n")

    fx = [f for f in a.fx.split(",") if f]
    for vi, v in enumerate(a.views):
        send("cmd freecam at " + v)
        time.sleep(1.2)
        for case in ["all"] + fx:
            if case != "all":
                send("cmd fx %s 0" % case)
            time.sleep(0.4)
            send("shot " + os.path.join(SHOTS, "%s_v%d_%s.bmp" % (a.tag, vi, case)).replace("\\", "/"))
            time.sleep(0.3)
            if case != "all":
                send("cmd fx %s 1" % case)
    send("cmd freecam off")
    time.sleep(0.5)


if __name__ == "__main__":
    main()
