"""Flicker as the view turns: a level started (movies skipped), the player held by noclip, the free camera turned at a
place along a path (the same path for each case) while the game records the screen, once with everything on and
once with each case's effects off. Each clip's frames are matched to the frame before by their motion (optical
flow: a turning view moves everything smoothly) and what is left over is what changed by itself: a surface's
highlights or a texture's pixels shimmering, the sky's flicker. Maps of it per case beside a frame, and the means.

    python tools/pan_probe.py <level> --place "x y z yaw0 yaw1 pitch" [--place ...] [--cases materials,taa]
        [--seconds 5] [--at 40] [--tag pan]

A case is one effect or several joined by "+" ("materials+taa"); "all" (everything on) is always taken first.
Clips: work/pan/<tag>_p<n>_<case>.mp4; maps work/pan/<tag>_p<n>_<case>_flicker.png.
"""
import argparse
import os
import subprocess
import sys
import time

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive  # noqa: E402
from level_review import EXE_DIR, START, WORK, read_log  # noqa: E402
from shoot import FFMPEG  # noqa: E402


def frames(clip, scale=2):
    """the clip's frames in grey, halved (the flow's noise less), repeated frames (a stall written twice) left out"""
    w, h = 2560 // scale, 1440 // scale
    p = subprocess.Popen([FFMPEG, "-loglevel", "error", "-i", clip, "-vf", "scale=%d:%d" % (w, h), "-f", "rawvideo",
                          "-pix_fmt", "gray", "-"], stdout=subprocess.PIPE)
    last = None
    while True:
        b = p.stdout.read(w * h)
        if len(b) < w * h:
            break
        f = np.frombuffer(b, np.uint8).reshape(h, w)
        if last is not None and np.array_equal(f, last):
            continue
        last = f
        yield f
    p.wait()


def measure(clip):
    dis = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
    prev = None
    acc = None
    n = 0
    for f in frames(clip):
        if prev is not None:
            flow = dis.calc(f, prev, None)  # where each of f's pixels was in prev
            h, w = f.shape
            gx, gy = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
            warped = cv2.remap(prev, gx + flow[..., 0], gy + flow[..., 1], cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
            r = np.abs(f.astype(np.float32) - warped.astype(np.float32))
            r[:, :8] = r[:, -8:] = 0  # (the view's edges: what turns in has no past)
            acc = r if acc is None else acc + r
            n += 1
        prev = f
    return (acc / max(n, 1)) if acc is not None else None, n, prev


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("level", type=int)
    ap.add_argument("--place", action="append", default=[])
    ap.add_argument("--look", type=int, default=0, help="the player's own view turned by mouse looks (this many a frame), not the free camera")
    ap.add_argument("--cases", default="materials,taa")
    ap.add_argument("--seconds", type=float, default=5)
    ap.add_argument("--at", type=float, default=40)
    ap.add_argument("--tag", default="pan")
    ap.add_argument("--measure-only", action="store_true")
    ap.add_argument("--off", default="film_grain", help="effects off through the whole run (film grain: noise of its own)")
    ap.add_argument("--no-noclip", action="store_true", help="the player left where it is (the free camera alone)")
    a = ap.parse_args()
    out = os.path.join(WORK, "pan")
    os.makedirs(out, exist_ok=True)
    cases = ["all"] + [c for c in a.cases.split(",") if c]
    clips = []
    if a.look:
        a.place = ["view"]
    for n in range(len(a.place)):
        for case in cases:
            clips.append((n, case, os.path.join(out, "%s_p%d_%s.mp4" % (a.tag, n, case.replace("+", "_"))).replace("\\", "/")))
    if not a.measure_only:
        control = os.path.join(WORK, a.tag + ".control.txt")
        open(control, "w").close()
        replay = os.path.join(WORK, a.tag + ".replay.txt")
        drive.build_replay(START, replay)
        with open(replay, "a") as f:
            f.write("%.1f 0 1 0000 0 0 0 0 0 0 0 0 0 0 0 0\n" % 600000)
        drive.kill_build_exe()
        env = dict(os.environ)
        env.update({"XBE_CONTROL": control, "XBE_INPUT_REPLAY": replay, "XBE_SKIP_MOVIES": "1", "XBE_FFMPEG": FFMPEG})
        subprocess.Popen([os.path.join(EXE_DIR, "CODFH.exe")], cwd=EXE_DIR, env=env)
        time.sleep(3)
        t0 = time.time()
        while time.time() - t0 < 600 and "d3d12: shader cache:" not in read_log():
            time.sleep(0.5)
        t0 = time.time() - 1

        def send(line):
            with open(control, "a") as f:
                f.write(line + "\n")

        while time.time() - t0 < 10:
            time.sleep(0.05)
        send("cmd level %d" % a.level)
        while time.time() - t0 < a.at:
            time.sleep(0.05)
        send("cmd god on" if a.no_noclip or a.look else "cmd noclip on")
        time.sleep(1.0)
        send("cmd hud 0")
        for c in a.off.split(","):
            if c:
                send("cmd fx %s 0" % c)
        for n, case, clip in clips:
            if case != "all":
                for c in case.split("+"):
                    send("cmd fx %s 0" % c)
            if a.look:
                time.sleep(2.5)
                if os.path.exists(clip):
                    os.remove(clip)
                send("record " + clip)
                time.sleep(0.3)
                t1 = time.time()
                k = 0
                while time.time() - t1 < a.seconds:
                    # (back and forth: the view stays about the same place for every case)
                    send("cmd look %d 0" % (a.look if (k // 60) % 2 == 0 else -a.look))
                    k += 1
                    time.sleep(1 / 60)
                time.sleep(0.3)
                send("record stop")
                time.sleep(1.0)
                if case != "all":
                    for c in case.split("+"):
                        send("cmd fx %s 1" % c)
                continue
            x, y, z, y0, y1, pitch = map(float, a.place[n].split())
            send("cmd freecam at %.1f %.1f %.1f %.2f %.2f" % (x, y, z, y0, pitch))
            time.sleep(2.5)  # (streamed in, the temporal history settled)
            if os.path.exists(clip):
                os.remove(clip)
            send("record " + clip)
            time.sleep(0.3)
            send("cmd freecam path %.1f %.1f %.1f %.2f %.2f %.1f %.1f %.1f %.2f %.2f %.2f" % (
                x, y, z, y0, pitch, x, y, z, y1, pitch, a.seconds))
            time.sleep(a.seconds + 0.3)
            send("record stop")
            time.sleep(1.0)
            if case != "all":
                for c in case.split("+"):
                    send("cmd fx %s 1" % c)
        time.sleep(1.0)
        drive.kill_build_exe()
    for n, case, clip in clips:
        if not os.path.exists(clip):
            print("p%d %-16s missing" % (n, case))
            continue
        m, count, last = measure(clip)
        if m is None:
            print("p%d %-16s no frames" % (n, case))
            continue
        base = clip[:-4]
        cv2.imwrite(base + "_flicker.png", np.clip(m * 10, 0, 255).astype(np.uint8))
        cv2.imwrite(base + "_frame.png", last)
        print("p%d %-16s mean %.3f  p99 %.2f  over 8: %.3f%%  (%d frames)" % (
            n, case, m.mean(), np.percentile(m, 99), 100 * (m > 8).mean(), count), flush=True)


if __name__ == "__main__":
    main()
