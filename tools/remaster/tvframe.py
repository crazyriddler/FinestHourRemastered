"""The television the 4:3 screens (menus, briefings, movies) play in on a 16:9 display: a photograph of a real set
(Unsplash, "An old television set sits on a wooden shelf", unsplash.com/photos/NGuLnUBFMbc, Unsplash License), its
glass cut out where the game's picture goes (a clean rounded rectangle fitted to it), the tube's edges in shadow and a
soft sheen over it. The set is placed with its screen in the middle, 86% of the height. Writes <pack>/ui/tv_frame.dds
(BC7) and <pack>/ui/tv_frame.txt, the screen's rectangle in the picture's 0..1 (x0 y0 x1 y1) the runtime reads.

    python -I tvframe.py <photo> <pack folder>
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate  # noqa: E402

W, H = 3840, 2160
SCREEN_H = 0.86


def gblur(a, sigma):
    h, w = a.shape
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.rfftfreq(w)[None, :]
    k = np.exp(-2 * (np.pi * sigma) ** 2 * (fx * fx + fy * fy))
    return np.fft.irfft2(np.fft.rfft2(a) * k, s=(h, w)).astype(np.float32)


# the glass's box in the photograph (pixels), a little larger than the glass: the bezel inside it is near black
GLASS_BOX = (700, 540, 2690, 2090)


def grow(seed, allowed):
    """the part of allowed connected to seed (4-neighbour dilation until nothing changes)"""
    r = seed & allowed
    while True:
        n = r.copy()
        n[1:] |= r[:-1]
        n[:-1] |= r[1:]
        n[:, 1:] |= r[:, :-1]
        n[:, :-1] |= r[:, 1:]
        n &= allowed
        if (n == r).all():
            return r
        r = n


def glass_mask(photo):
    """the screen's glass: what is not near black inside GLASS_BOX, reached from its middle, holes filled (worked
    out at a quarter of the size, then brought back up smooth)"""
    q = 4
    small = photo.convert("L").resize((photo.width // q, photo.height // q), Image.BILINEAR)
    g = np.asarray(small, np.float32) / 255
    bx0, by0, bx1, by1 = (v // q for v in GLASS_BOX)
    open_ = gblur(g, 1) > 0.13
    box = np.zeros_like(open_)
    box[by0:by1, bx0:bx1] = True
    open_ &= box
    seed = np.zeros_like(open_)
    seed[(by0 + by1) // 2, (bx0 + bx1) // 2] = True
    region = grow(seed, open_)
    edge = np.zeros_like(region)
    edge[0, :] = edge[-1, :] = True
    edge[:, 0] = edge[:, -1] = True
    outside = grow(edge, ~region)
    filled = (~outside).astype(np.float32)
    up = np.asarray(Image.fromarray((filled * 255).astype(np.uint8)).resize(photo.size, Image.BICUBIC), np.float32) / 255
    return gblur(up, 24) > 0.38  # closed: notches left by reflections at the edge filled


def rounded_box(x, y, a, b, r):
    """signed distance (pixels, negative inside) to the rounded rectangle of half sizes a, b and corner radius r"""
    qx, qy = np.abs(x) - (a - r), np.abs(y) - (b - r)
    return np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - r


def fit_glass(mask):
    """the glass as a rounded rectangle: its sides where the mask's middle rows and columns end, the corners' radius
    that matches the mask best, grown a little over the mask's ragged edge (what is left outside goes into the
    bezel's shadow)"""
    # the sides where the middle rows and columns of the mask end (a stray pixel or a notch moves nothing)
    h, w = mask.shape
    ys, xs = np.where(mask)
    mid_y, mid_x = int(np.median(ys)), int(np.median(xs))
    span = lambda line: (np.argmax(line), len(line) - np.argmax(line[::-1]))
    rows = [span(mask[y]) for y in range(mid_y - h // 8, mid_y + h // 8) if mask[y].any()]
    cols = [span(mask[:, x]) for x in range(mid_x - w // 8, mid_x + w // 8) if mask[:, x].any()]
    x0, x1 = np.median([r[0] for r in rows]), np.median([r[1] for r in rows])
    y0, y1 = np.median([c[0] for c in cols]), np.median([c[1] for c in cols])
    cx, cy, a, b = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2
    # the corners' radius that matches the mask best (intersection over union), at a quarter of the size
    q = 4
    sm = mask[::q, ::q]
    yy, xx = np.mgrid[0:sm.shape[0], 0:sm.shape[1]]
    best = (-1, 0)
    for r in np.arange(20, min(a, b), 8):
        box = rounded_box(xx * q + 0.5 - cx, yy * q + 0.5 - cy, a, b, r) <= 0
        best = max(best, ((box & sm).sum() / (box | sm).sum(), r))
    r = float(best[1])
    d = rounded_box(xs + 0.5 - cx, ys + 0.5 - cy, a, b, r)
    g = float(np.clip(np.percentile(d, 99.5), 0, 0.006 * h))
    return cx, cy, a + g, b + g, r + g


def main():
    photo_path, pack = sys.argv[1:3]
    photo = Image.open(photo_path).convert("RGB")
    mask = glass_mask(photo)
    cx, cy, ga, gb, gr = fit_glass(mask)
    print("glass", cx, cy, ga, gb, "corner", gr)
    k = SCREEN_H * H / (2 * gb)
    pw, ph = int(photo.width * k), int(photo.height * k)
    big = photo.resize((pw, ph), Image.LANCZOS)
    mbig = Image.fromarray((mask * 255).astype(np.uint8)).resize((pw, ph), Image.LANCZOS)
    ox, oy = int(round(W / 2 - cx * k)), int(round(H / 2 - cy * k))
    canvas = Image.new("RGB", (W, H), (0, 0, 0))
    canvas.paste(big, (ox, oy))
    mcan = Image.new("L", (W, H), 0)
    mcan.paste(mbig, (ox, oy))
    col = np.asarray(canvas, np.float32)
    photo_glass = np.asarray(mcan, np.float32) / 255
    gx, gy, a, b = cx * k + ox, cy * k + oy, ga * k, gb * k
    sx0, sy0, sx1, sy1 = (gx - a) / W, (gy - b) / H, (gx + a) / W, (gy + b) / H

    # the room darker towards the edges
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    vig = 1 - 0.55 * np.clip(((xx / W - 0.5) * 1.3) ** 2 + ((yy / H - 0.5) * 1.5) ** 2, 0, 1)
    col *= vig[..., None]
    # what is left of the photograph's glass outside the shape: in the bezel's shadow
    col *= 1 - 0.9 * photo_glass[..., None]

    # the glass's signed distance (pixels, negative inside)
    u, v = (xx + 0.5 - gx) / a, (yy + 0.5 - gy) / b
    d = rounded_box(xx + 0.5 - gx, yy + 0.5 - gy, a, b, gr * k)
    inside = np.clip(0.5 - d, 0, 1)
    # the tube's edge in shadow, a broad soft sheen high on the left
    depth = np.clip(-d / (0.03 * H), 0, 1)
    sm = depth * depth * (3 - 2 * depth)
    shadow = 0.92 * (1 - sm) ** 2
    sheen = 0.07 * np.exp(-((u + 0.35) ** 2 / 0.12 + (v + 0.55) ** 2 / 0.04))
    ga_ = shadow + sheen * (1 - shadow)
    grgb = np.where(ga_ > 1e-6, 255 * sheen * (1 - shadow) / np.maximum(ga_, 1e-6), 0)
    alpha = (1 - inside) + inside * ga_
    rgb = (col * (1 - inside)[..., None] + (inside * ga_ * grgb)[..., None]) / np.maximum(alpha, 1e-6)[..., None]
    out = np.dstack([np.clip(rgb, 0, 255), np.clip(alpha, 0, 1) * 255]).astype(np.uint8)

    work = os.path.join(pack, "work_ui")
    os.makedirs(work, exist_ok=True)
    png = os.path.join(work, "tv_frame.png")
    Image.fromarray(out, "RGBA").save(png)
    Image.fromarray(out, "RGBA").resize((W // 4, H // 4), Image.LANCZOS).save(os.path.join(work, "tv_frame_preview.png"))
    os.makedirs(os.path.join(pack, "ui"), exist_ok=True)
    generate.texconv([png], "BC7_UNORM", os.path.join(pack, "ui"))
    with open(os.path.join(pack, "ui", "tv_frame.txt"), "w") as f:
        f.write("%.5f %.5f %.5f %.5f\n" % (sx0, sy0, sx1, sy1))
    with open(os.path.join(pack, "ui", "tv_frame_credit.txt"), "w") as f:
        f.write("Television photograph: \"An old television set sits on a wooden shelf\", Unsplash "
                "(unsplash.com/photos/NGuLnUBFMbc), used under the Unsplash License.\n")
    print("screen", sx0, sy0, sx1, sy1)


if __name__ == "__main__":
    main()
