"""The tank's vision slit glass (T34_glass_png, drawn over the whole screen in the tank's view) again, smooth: the
original is 128 x 256 with its grime in a dithered alpha, which stretched over the screen showed as blocks and
speckles. The dither melted (a wide blur in the middle, a narrow one by the rim so its lines stay), brought up 4
times smoothly, a soft smudge of grime over it, so the glass reads as dirty glass at any size. BC7 with mips.

    python -I tank_glass.py <extract folder> <pack folder>
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate  # noqa: E402

GLASS = "3e4d009ca3efd1d1"
K = 4


def blur(a, sigma):
    """separable gaussian over the first two axes, edges clamped"""
    if sigma <= 0:
        return a
    r = int(np.ceil(sigma * 3))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    out = a
    for axis in (0, 1):
        pad = [(0, 0)] * a.ndim
        pad[axis] = (r, r)
        p = np.pad(out, pad, mode="edge")
        acc = np.zeros_like(out)
        for i, w in enumerate(k):
            sl = [slice(None)] * a.ndim
            sl[axis] = slice(i, i + out.shape[axis])
            acc += w * p[tuple(sl)]
        out = acc
    return out


def smooth_noise(h, w, cell, seed):
    """value noise, smooth, 0 to 1, features about cell pixels"""
    rng = np.random.default_rng(seed)
    gh, gw = h // cell + 2, w // cell + 2
    g = rng.random((gh, gw)).astype(np.float32)
    return np.asarray(Image.fromarray(g).resize((gw * cell, gh * cell), Image.BICUBIC))[:h, :w]


def glass(src):
    h, w = src.shape[:2]
    a = src[..., 3:4] / 255.0
    pc = src[..., :3] * a  # premultiplied: the colour of what is there
    # how far from the rim (texels): the rim's lines kept sharper, the middle's dither melted
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    edge = np.minimum(np.minimum(xx, w - 1 - xx), np.minimum(yy, h - 1 - yy))
    inner = np.clip((edge - 2) / 5, 0, 1)[..., None]
    rgba = np.concatenate([pc, a], axis=2)
    soft = blur(rgba, 1.8)
    sharp = blur(rgba, 0.9)
    rgba = sharp * (1 - inner) + soft * inner
    big = np.stack([np.asarray(Image.fromarray(rgba[..., c].astype(np.float32)).resize((w * K, h * K), Image.BICUBIC))
                    for c in range(4)], axis=2)
    H, W = big.shape[:2]
    # a soft smudge over the grime, more in the middle than at the rim
    n = 0.6 * smooth_noise(H, W, 48, 1) + 0.4 * smooth_noise(H, W, 16, 2)
    inner_big = np.asarray(Image.fromarray(inner[..., 0]).resize((W, H), Image.BILINEAR))[..., None]
    big[..., 3:4] *= 1 + (n[..., None] - 0.5) * 0.5 * inner_big
    big[..., :3] *= 1 + (n[..., None] - 0.5) * 0.5 * inner_big
    alpha = np.clip(big[..., 3:4], 0, 1)
    col = np.where(alpha > 1e-4, big[..., :3] / np.maximum(alpha, 1e-4), 0)
    return np.concatenate([np.clip(col, 0, 255), alpha * 255], axis=2)


def main():
    src_dir, pack = sys.argv[1], sys.argv[2]
    work = os.path.join(pack, "work_misc")
    os.makedirs(work, exist_ok=True)
    src = np.asarray(Image.open(os.path.join(src_dir, GLASS + ".png")).convert("RGBA")).astype(np.float32)
    out = glass(src)
    p = os.path.join(work, GLASS + ".c.png")
    Image.fromarray((out + 0.5).astype(np.uint8), "RGBA").save(p)
    print(GLASS, out.shape[1], "x", out.shape[0])
    generate.texconv([p], "BC7_UNORM", os.path.join(pack, "textures"))


if __name__ == "__main__":
    main()
