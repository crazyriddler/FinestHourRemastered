"""Water Interaction's field read back (XBE_WAVE_DUMP: logs/wave_water.f32, wave_cover.f32, wave_state.f32) as one
picture: the water's height, the cover, the field's height (red up, blue down).

    python tools/wave_dump.py [out.png]
"""
import os
import sys

import numpy as np
from PIL import Image

LOGS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "out", "build", "release", "logs")


def read(name, dtype, ch):
    b = open(os.path.join(LOGS, "wave_%s.f32" % name), "rb").read()
    w, h, pitch = np.frombuffer(b[:12], np.uint32)
    rows = np.frombuffer(b[12:12 + pitch * h], np.uint8).reshape(h, pitch)
    return rows[:, :w * ch * np.dtype(dtype).itemsize].copy().view(dtype).reshape(h, w, ch).astype(np.float32)


def main():
    water = read("water", np.float32, 1)[..., 0]
    cover = read("cover", np.float16, 1)[..., 0]
    state = read("state", np.float16, 4)
    wet = water > -1e8
    print("water: %.1f%% wet, height %s" % (wet.mean() * 100, (water[wet].min(), water[wet].max()) if wet.any() else "-"))
    print("cover: %.1f%% > 0, max %.1f" % ((cover > 0).mean() * 100, cover.max()))
    print("field: |h| max %.2f, mean %.3f, wet flag %.1f%%" % (np.abs(state[..., 0]).max(), np.abs(state[..., 0]).mean(), (state[..., 3] > 0).mean() * 100))
    a = np.where(wet, 0.25, 0)
    img1 = np.dstack([a, a, a])
    img2 = np.dstack([np.zeros_like(cover), np.clip(cover / 175, 0, 1), np.where(wet, 0.15, 0)])
    hh = state[..., 0]
    img3 = np.dstack([np.clip(hh * 0.3, 0, 1), np.where(wet, 0.1, 0), np.clip(-hh * 0.3, 0, 1)])
    out = np.concatenate([img1, img2, img3], 1)
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(LOGS, "..", "..", "..", "..", "work", "shots", "wave_dump.png")
    Image.fromarray((out * 255).astype(np.uint8)).save(path)
    print(path)


if __name__ == "__main__":
    main()
