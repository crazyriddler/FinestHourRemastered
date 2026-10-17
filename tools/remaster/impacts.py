"""The bullets' impacts (the dirt they throw up: dirtgeyser 1 to 3, bulletgroundhit) as dust: the game's dark streaks,
sharpened by the upscaler, pile up under a machine gun's burst into a black stain. Their shape kept, their colour a
light earth dust, a little more see-through. From the pack's own textures as they were (alpha_backup), BC7 with mips.

    python -I impacts.py <pack>
"""
import os
import shutil
import sys
import tempfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate  # noqa: E402

IMPACTS = ["d671f0d093f9f6af", "3ef5763b805afcb0", "9d1e433f62fe9d40", "9d7bf44df07cccc3"]


def dust(img):
    px = np.asarray(img.convert("RGBA")).astype(np.float32) / 255
    rgb, a = px[..., :3], px[..., 3]
    lum = rgb @ np.array([0.299, 0.587, 0.114], np.float32)
    # light earth, its darkest bits a mid brown: the texture's own shading kept, lifted
    shade = 0.55 + 0.45 * np.clip(lum / max(np.percentile(lum[a > 0.1], 95) if (a > 0.1).any() else 1, 1e-3), 0, 1)
    earth = np.array([0.66, 0.58, 0.47], np.float32)
    col = earth[None, None, :] * shade[..., None]
    alpha = a * 0.7
    return Image.fromarray((np.dstack([col, alpha]) * 255 + 0.5).clip(0, 255).astype(np.uint8), "RGBA")


def main():
    pack = sys.argv[1]
    tex = os.path.join(pack, "textures")
    backup = os.path.join(os.path.dirname(os.path.abspath(pack)), "alpha_backup")
    work = tempfile.mkdtemp(prefix="impacts_")
    pngs = []
    for rid in IMPACTS:
        src = os.path.join(backup, rid + ".c.dds")
        if not os.path.exists(src):
            os.makedirs(backup, exist_ok=True)
            shutil.copy2(os.path.join(tex, rid + ".c.dds"), src)
        out = os.path.join(work, rid + ".c.png")
        dust(Image.open(src)).save(out)
        pngs.append(out)
    generate.texconv(pngs, "BC7_UNORM", tex)
    shutil.rmtree(work, ignore_errors=True)
    print("impacts:", len(pngs))


if __name__ == "__main__":
    main()
