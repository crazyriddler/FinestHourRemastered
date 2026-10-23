"""Brightness pulsing in a recorded camera pan (tools/pan_probe.py's clips): each frame against the mean of the one
before and the one after, both moved onto it by their motion (optical flow), smoothed over a few pixels (a surface
lit a little brighter, then darker, frame after frame; not an edge's sub-pixel error): its mean, and the share of
the picture pulsing by more than 2 (0..255).

    python tools/pulse_metric.py <clip.mp4> [...]
"""
import os
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pan_probe import frames  # noqa: E402


def warp(src, dst, dis):
    flow = dis.calc(dst.astype(np.uint8), src.astype(np.uint8), None)
    h, w = dst.shape
    gx, gy = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
    return cv2.remap(src, gx + flow[..., 0], gy + flow[..., 1], cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)


def main():
    for clip in sys.argv[1:]:
        fs = [f.astype(np.float32) for f in frames(clip, 2)]
        sharp = np.array([cv2.Laplacian(f, cv2.CV_32F).var() for f in fs])
        ok = sharp > 0.6 * np.median(sharp)
        dis = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
        acc, over, n = 0.0, 0.0, 0
        for i in range(1, len(fs) - 1):
            if not (ok[i - 1] and ok[i] and ok[i + 1]):
                continue
            mid = 0.5 * (warp(fs[i - 1], fs[i], dis) + warp(fs[i + 1], fs[i], dis))
            p = np.abs(cv2.GaussianBlur(fs[i] - mid, (0, 0), 4))
            p[:, :12] = p[:, -12:] = 0
            acc += float(p.mean())
            over += float((p > 2).mean())
            n += 1
        print("%-36s %3d frames  pulse %.3f  over 2: %.2f%%" % (os.path.basename(clip), n, acc / max(n, 1), 100 * over / max(n, 1)), flush=True)


if __name__ == "__main__":
    main()
