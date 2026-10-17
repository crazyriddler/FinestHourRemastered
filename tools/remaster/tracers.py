"""The tracers' sprites (the round and the streak) as incandescent gold: the upscaler left a grey-brown, smoky halo
about their bright core, which over a dark scene reads as a dark stain. Their shape and softness are kept: how bright
and how opaque each pixel was makes how much it glows; its colour goes from deep gold at the edge to white gold at the
core, nothing grey. From the pack's own textures as they were (alpha_backup), BC7 with mips.

    python -I tracers.py <pack>
"""
import os
import shutil
import sys
import tempfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate  # noqa: E402

TRACERS = ["34d4d655e55e0eae", "093ae31e2da4082d"]  # tracerroundwhitetrans, tracerstreakwhitetrans


def gold(img):
    px = np.asarray(img.convert("RGBA")).astype(np.float32) / 255
    rgb, a = px[..., :3], px[..., 3]
    lum = rgb @ np.array([0.299, 0.587, 0.114], np.float32)
    # the glow: brightness where it was opaque, scaled so the core reaches 1
    glow = lum * a
    glow = glow / max(np.percentile(glow, 99.5), 1e-3)
    glow = np.clip(glow, 0, 1)
    edge = np.array([1.0, 0.62, 0.18], np.float32)   # deep gold
    mid = np.array([1.0, 0.82, 0.42], np.float32)    # gold
    core = np.array([1.0, 0.97, 0.85], np.float32)   # white gold
    t = glow[..., None]
    col = np.where(t < 0.5, edge + (mid - edge) * (t / 0.5), mid + (core - mid) * ((t - 0.5) / 0.5))
    # opacity: the glow's, eased so the faint halo stays faint (no flat rim)
    alpha = np.clip(glow * 1.15, 0, 1) ** 1.25
    return Image.fromarray((np.dstack([col, alpha]) * 255 + 0.5).clip(0, 255).astype(np.uint8), "RGBA")


def main():
    pack = sys.argv[1]
    tex = os.path.join(pack, "textures")
    backup = os.path.join(os.path.dirname(os.path.abspath(pack)), "alpha_backup")
    work = tempfile.mkdtemp(prefix="tracers_")
    pngs = []
    for rid in TRACERS:
        src = os.path.join(backup, rid + ".c.dds")
        if not os.path.exists(src):
            src = os.path.join(tex, rid + ".c.dds")
        out = os.path.join(work, rid + ".c.png")
        gold(Image.open(src)).save(out)
        pngs.append(out)
    generate.texconv(pngs, "BC7_UNORM", tex)
    shutil.rmtree(work, ignore_errors=True)
    print("tracers:", len(pngs))


if __name__ == "__main__":
    main()
