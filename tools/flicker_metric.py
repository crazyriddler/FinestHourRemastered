"""Flicker in a recorded camera pan (tools/pan_probe.py's clips), told from the image's own detail: each frame matched
to the one before by its motion (optical flow), what is left over and the frame itself high-passed (the pixel-scale
changes a shimmer makes, not the flow's slow errors), their ratio over the frames that are sharp (frames the game
blurs itself, a shell shock's, left out). A clip that shimmers has more left over for its detail.

    python tools/flicker_metric.py <clip.mp4> [...] [--box x0 y0 x1 y1]
"""
import argparse
import os
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pan_probe import frames  # noqa: E402


def hp(img):
    return img - cv2.GaussianBlur(img, (0, 0), 1.5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("clips", nargs="+")
    ap.add_argument("--box", nargs=4, type=int, default=None)
    a = ap.parse_args()
    for clip in a.clips:
        fs = [f.astype(np.float32) for f in frames(clip, 1)]
        if a.box:
            x0, y0, x1, y1 = a.box
            fs = [f[y0:y1, x0:x1] for f in fs]
        sharp = np.array([cv2.Laplacian(f, cv2.CV_32F).var() for f in fs])
        ok = sharp > 0.6 * np.median(sharp)
        dis = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
        res, det, n = 0.0, 0.0, 0
        for i in range(1, len(fs)):
            if not (ok[i] and ok[i - 1]):
                continue
            f, prev = fs[i], fs[i - 1]
            flow = dis.calc(f.astype(np.uint8), prev.astype(np.uint8), None)
            h, w = f.shape
            gx, gy = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
            warped = cv2.remap(prev, gx + flow[..., 0], gy + flow[..., 1], cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
            r = hp(f) - hp(warped)
            r[:, :16] = r[:, -16:] = 0
            res += float(np.abs(r).mean())
            det += float(np.abs(hp(f)).mean())
            n += 1
        print("%-40s %3d sharp pairs  left over %.3f  detail %.3f  ratio %.3f" % (
            os.path.basename(clip), n, res / max(n, 1), det / max(n, 1), res / max(det, 1e-6)), flush=True)


if __name__ == "__main__":
    main()
