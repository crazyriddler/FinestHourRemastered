"""The pack's colour textures with transparency, cleaned where they are see-through: the upscaler's colours in the
transparent parts (noise of every colour, at an alpha of a few in 255, which filtering and the glow of the bright
sprites bring out as coloured fringes) replaced by the colours of the nearest solid parts; alpha of 4 in 255 or less
faded smoothly to 0 under 16. Textures whose faint outer rings came out blocky (the HUD's hit indicators) lose them, their edges smoothed.

    python -I alpha_clean.py <pack> [ids...]

The textures before are kept in <pack>/../alpha_backup. BC7 with mips again (texconv).
"""
import glob
import os
import shutil
import sys
import tempfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate  # noqa: E402

# blocky faint rings: anything under this alpha goes, the edge softened
RINGS = {"4287efaf571ca472": 40, "0e30e882653a1b13": 40}
# a faint square halo about a bright core (the tracers', which their glow brings out): faded out up to this alpha
HALOS = {"34d4d655e55e0eae": 24, "093ae31e2da4082d": 24}


def blur(arr, r):
    """a separable gaussian of radius r (PIL blurs no float images), edges clamped"""
    sigma = max(r, 0.5)
    n = int(sigma * 3) + 1
    k = np.exp(-0.5 * (np.arange(-n, n + 1) / sigma) ** 2)
    k /= k.sum()
    out = arr.astype(np.float32)
    for axis in (0, 1):
        pad = [(0, 0)] * out.ndim
        pad[axis] = (n, n)
        p = np.pad(out, pad, mode="edge")
        acc = np.zeros_like(out)
        for i, w in enumerate(k):
            sl = [slice(None)] * out.ndim
            sl[axis] = slice(i, i + out.shape[axis])
            acc += w * p[tuple(sl)]
        out = acc
    return out


def bleed(rgb, a):
    """rgb where alpha is low replaced by the alpha weighted average of the solid colours about it, wider and wider."""
    w = (a > 0.5).astype(np.float32)
    if w.sum() == 0:
        w = a.astype(np.float32)
    acc, wt = np.zeros_like(rgb), np.zeros_like(a)
    cur_rgb, cur_w = rgb * w[..., None], w
    out = rgb.copy()
    filled = w > 0
    for radius in (1, 2, 4, 8, 16, 32, 64):
        bw = blur(cur_w, radius)
        bc = blur(cur_rgb, radius)
        take = (~filled) & (bw > 1e-4)
        out[take] = bc[take] / bw[take][:, None]
        filled |= take
        if filled.all():
            break
    # the upscaler's colours stand down to an alpha of 16 in 255 (soft halos keep their own); the nearly clear ones,
    # its noise, take their neighbours'
    mix = np.clip(a / (16 / 255), 0, 1)[..., None]
    return rgb * mix + out * (1 - mix)


def gauss(arr, r):
    return blur(arr, r)


def black_under(pack, rid):
    """the game's own texture black where it is clear: a sprite drawn added to the scene (the moon, flashes), whose
    clear parts must stay black, not take the colours about them (added, the whole square showed)"""
    src = os.path.join(os.path.dirname(os.path.abspath(pack)), "extract", rid + ".png")
    if not os.path.exists(src):
        return False
    o = np.asarray(Image.open(src).convert("RGBA")).astype(np.float32)
    clear = o[..., 3] < 8
    return clear.mean() >= 0.05 and o[..., :3][clear].mean() <= 6


def clean(path, rid, black=False):
    img = Image.open(path).convert("RGBA")
    px = np.asarray(img).astype(np.float32) / 255
    rgb, a = px[..., :3], px[..., 3]
    if a.min() > 0.98:
        return None
    noisy = ((a > 0) & (a < 8 / 255)).mean()
    if rid not in RINGS and rid not in HALOS and noisy < 0.002 and not ((a < 0.5) & (a > 0)).any():
        return None
    if rid in RINGS:
        cut = RINGS[rid] / 255
        solid = (a > cut).astype(np.float32)
        # the edge antialiased: the solid shape's mask a little blurred, never more than the texture's own alpha
        soft = np.clip(gauss(solid, max(1.0, img.width / 256)) * 1.4 - 0.2, 0, 1)
        a = np.minimum(np.maximum(a * solid, soft * a.max()), np.where(solid > 0, a, soft * cut * 4))
        a = np.where(a < cut * 0.5, 0, a)
    if rid in HALOS:
        hi = HALOS[rid] / 255
        t = np.clip(a / hi, 0, 1)
        a = a * t * t * (3 - 2 * t)
    # the faintest fade smoothly to nothing (no ring where they would stop)
    t = np.clip((a * 255 - 2) / 14, 0, 1)
    a = np.where(a < 16 / 255, a * t * t * (3 - 2 * t), a)
    rgb = bleed(rgb, a)
    if black:
        rgb = rgb * np.clip(a / (24 / 255), 0, 1)[..., None]
    return Image.fromarray((np.dstack([rgb, a]) * 255 + 0.5).clip(0, 255).astype(np.uint8), "RGBA")


def main():
    pack = sys.argv[1]
    tex = os.path.join(pack, "textures")
    backup = os.path.join(os.path.dirname(os.path.abspath(pack)), "alpha_backup")
    os.makedirs(backup, exist_ok=True)
    ids = sys.argv[2:] or [os.path.basename(f)[:-6] for f in glob.glob(os.path.join(tex, "*.c.dds"))]
    work = tempfile.mkdtemp(prefix="alpha_clean_")
    pngs = []
    for i, rid in enumerate(ids):
        path = os.path.join(tex, rid + ".c.dds")
        kept = os.path.join(backup, rid + ".c.dds")
        try:
            out = clean(kept if os.path.exists(kept) else path, rid, black_under(pack, rid))  # from the pack's own, again
        except Exception as e:  # a format PIL does not read
            print("skipped", rid, e)
            continue
        if out is None:
            continue
        if not os.path.exists(os.path.join(backup, rid + ".c.dds")):
            shutil.copy2(path, os.path.join(backup, rid + ".c.dds"))
        png = os.path.join(work, rid + ".c.png")
        out.save(png)
        pngs.append(png)
        if i % 200 == 0:
            print(i, "of", len(ids), flush=True)
    print(len(pngs), "cleaned", flush=True)
    generate.texconv(pngs, "BC7_UNORM", tex)
    shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
