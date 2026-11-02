"""Frames that flash for one frame in a recording (a glitch: garbage, a piece missing or wrong for a frame): where a
frame differs from both its neighbours while they agree with each other. Prints the worst frames and saves each with
its neighbours and a map of where it flashed (<out>/flick_<n>.png).

    python tools/flicker_scan.py <video.mp4> [--out work/shots/flicker] [--top 12]
"""
import argparse
import os
import subprocess

import numpy as np
from PIL import Image

FF = os.environ.get("XBE_FFMPEG", "ffmpeg")


def frames(path, w, h):
    p = subprocess.Popen([FF, "-v", "error", "-i", path, "-vf", "scale=%d:%d" % (w, h), "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                         stdout=subprocess.PIPE)
    n = w * h * 3
    while True:
        b = p.stdout.read(n)
        if len(b) < n:
            break
        yield np.frombuffer(b, np.uint8).reshape(h, w, 3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video")
    ap.add_argument("--out", default=os.path.join("work", "shots", "flicker"))
    ap.add_argument("--top", type=int, default=12)
    a = ap.parse_args()
    W, H = 640, 360
    F = [f.astype(np.int16) for f in frames(a.video, W, H)]
    print(len(F), "frames")
    scores = []
    for t in range(1, len(F) - 1):
        a_, b_, c_ = F[t - 1], F[t], F[t + 1]
        if np.array_equal(a_, b_) or np.array_equal(b_, c_):
            continue  # (a frame written again for a slow one)
        d1 = np.abs(b_ - a_).max(2)
        d2 = np.abs(b_ - c_).max(2)
        d3 = np.abs(c_ - a_).max(2)
        spike = np.clip(np.minimum(d1, d2) - d3, 0, None)
        m = spike > 40
        scores.append((int(m.sum()), t, m))
    scores.sort(key=lambda s: -s[0])
    os.makedirs(a.out, exist_ok=True)
    for k, (n, t, m) in enumerate(scores[:a.top]):
        print("frame %d (%.2f s): %d pixels flash" % (t, t / 60, n))
        if n < 20:
            continue
        s = Image.new("RGB", (W * 2, H * 2))
        s.paste(Image.fromarray(F[t - 1].astype(np.uint8)), (0, 0))
        s.paste(Image.fromarray(F[t].astype(np.uint8)), (W, 0))
        s.paste(Image.fromarray(F[t + 1].astype(np.uint8)), (0, H))
        s.paste(Image.fromarray((m * 255).astype(np.uint8)).convert("RGB"), (W, H))
        s.save(os.path.join(a.out, "flick_%02d_f%d.png" % (k, t)))


if __name__ == "__main__":
    main()
