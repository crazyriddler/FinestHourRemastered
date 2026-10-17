"""The bullets' impact smoke (ysmoke) as a soft dust puff: the game's is a mid grey puff with an almost all-or-nothing
alpha, which by night, a machine gun's burst piling its puffs where the rounds land, reads as a black, jagged blot.
Its shape kept: the alpha feathered (blurred, eased to nothing at the edge) and lighter, its colour a pale earth dust
with the texture's own shading. From the pack's own texture as it was (alpha_backup), BC7 with mips.

    python -I smoke.py <pack>
"""
import os
import shutil
import sys
import tempfile

import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate  # noqa: E402

SMOKE = ["b3a23299277edaed"]  # ysmoke


def puff(img):
    img = img.convert("RGBA")
    w = img.size[0]
    px = np.asarray(img).astype(np.float32) / 255
    rgb = px[..., :3]
    # the alpha feathered: blurred by a fortieth of the sprite, eased so the rim fades to nothing
    a = Image.fromarray((px[..., 3] * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(w / 40))
    a = np.asarray(a).astype(np.float32) / 255
    a = np.clip((a - 0.08) / 0.92, 0, 1)
    a = a * a * (3 - 2 * a) * 0.6
    lum = rgb @ np.array([0.299, 0.587, 0.114], np.float32)
    inside = px[..., 3] > 0.5
    ref = np.percentile(lum[inside], 90) if inside.any() else 1
    shade = 0.6 + 0.4 * np.clip(lum / max(ref, 1e-3), 0, 1)
    dust = np.array([0.78, 0.72, 0.63], np.float32)
    col = dust[None, None, :] * shade[..., None]
    return Image.fromarray((np.dstack([col, a]) * 255 + 0.5).clip(0, 255).astype(np.uint8), "RGBA")


def main():
    pack = sys.argv[1]
    tex = os.path.join(pack, "textures")
    backup = os.path.join(os.path.dirname(os.path.abspath(pack)), "alpha_backup")
    work = tempfile.mkdtemp(prefix="smoke_")
    pngs = []
    for rid in SMOKE:
        src = os.path.join(backup, rid + ".c.dds")
        if not os.path.exists(src):
            os.makedirs(backup, exist_ok=True)
            shutil.copy2(os.path.join(tex, rid + ".c.dds"), src)
        out = os.path.join(work, rid + ".c.png")
        puff(Image.open(src)).save(out)
        pngs.append(out)
    generate.texconv(pngs, "BC7_UNORM", tex)
    shutil.rmtree(work, ignore_errors=True)
    print("smoke:", len(pngs))


if __name__ == "__main__":
    main()
