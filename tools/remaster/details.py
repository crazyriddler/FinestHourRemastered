"""Detail normal maps the renderer tiles over remastered surfaces close up, one per kind in materials.DETAILS:
procedural height fields made tileable by building them in the frequency domain, then normals from their slopes.

    python -I details.py <pack folder>
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate  # noqa: E402
import materials  # noqa: E402

N = 512
rng = np.random.default_rng(1944)


def spectrum_noise(power, aniso=(1.0, 1.0)):
    """Tileable noise with amplitude falling off as 1/f^power; aniso stretches it along an axis."""
    fx = np.fft.fftfreq(N)[None, :] * aniso[0]
    fy = np.fft.fftfreq(N)[:, None] * aniso[1]
    f = np.sqrt(fx * fx + fy * fy)
    f[0, 0] = 1
    amp = 1 / f ** power
    amp[0, 0] = 0
    phase = rng.uniform(0, 2 * np.pi, (N, N))
    h = np.real(np.fft.ifft2(amp * np.exp(1j * phase)))
    return (h - h.mean()) / (h.std() + 1e-9)


def grid_cells(count):
    """about count tileable points, one jittered in each square of a grid: distances to the nearest and second
    nearest (pixels) and a value of the nearest's (-1..1); each pixel looks at its square's 3 x 3 neighbourhood"""
    g = max(2, int(round(np.sqrt(count))))
    c = N / g
    px = (np.arange(g)[None, :] + rng.uniform(0.05, 0.95, (g, g))) * c
    py = (np.arange(g)[:, None] + rng.uniform(0.05, 0.95, (g, g))) * c
    val = rng.uniform(-1, 1, (g, g)).astype(np.float32)
    yy, xx = np.mgrid[0:N, 0:N].astype(np.float32) + 0.5
    ci, cj = (yy // c).astype(int), (xx // c).astype(int)
    f1 = np.full((N, N), 1e9, np.float32)
    f2 = np.full((N, N), 1e9, np.float32)
    v = np.zeros((N, N), np.float32)
    for di in (-1, 0, 1):
        for dj in (-1, 0, 1):
            i, j = (ci + di) % g, (cj + dj) % g
            dx = np.abs(xx - px[i, j])
            dy = np.abs(yy - py[i, j])
            d = np.sqrt(np.minimum(dx, N - dx) ** 2 + np.minimum(dy, N - dy) ** 2)
            closer = d < f1
            f2 = np.where(closer, f1, np.minimum(f2, d))
            v = np.where(closer, val[i, j], v)
            f1 = np.minimum(f1, d)
    return f1, f2, v


def voronoi(count):
    """Distances to the nearest and second nearest of count tileable points, in pixels"""
    f1, f2, _ = grid_cells(count)
    return f1, f2


def highpass(h, sigma):
    """h without its broad shapes (a gaussian of sigma pixels taken off, tileable): only fine detail, no blotches"""
    fx = np.fft.fftfreq(N)[None, :]
    fy = np.fft.fftfreq(N)[:, None]
    low = np.exp(-2 * (np.pi * sigma) ** 2 * (fx * fx + fy * fy))
    return np.real(np.fft.ifft2(np.fft.fft2(h) * (1 - low)))


def norm(h):
    return (h - h.mean()) / (h.std() + 1e-9)


def stones(count, gap):
    """Rounded stones with cracks between them"""
    f1, f2 = voronoi(count)
    edge = f2 - f1
    return norm(np.sqrt(np.clip(edge / gap, 0, 1)))


def bumps(count, radius, sign=1.0):
    """Smooth round bumps (or dents with sign -1)"""
    f1, _ = voronoi(count)
    return norm(sign * np.exp(-(f1 / radius) ** 2))


def height(kind):
    if kind == "stone":
        return 0.8 * stones(220, 10) + 0.35 * spectrum_noise(1.3)
    if kind == "wood":
        return 1.0 * spectrum_noise(2.0, (10.0, 0.2)) + 0.15 * spectrum_noise(1.4)
    if kind == "fabric":
        y, x = np.mgrid[0:N, 0:N] * (2 * np.pi * 96 / N)
        weave = np.sin(x) * np.sign(np.sin(y / 2)) + np.sin(y) * np.sign(np.sin(x / 2))
        return 0.7 * weave + 0.3 * spectrum_noise(1.2)
    if kind == "metal":
        return 0.5 * spectrum_noise(2.2, (0.1, 8.0)) + 0.15 * bumps(40, 14, -1) + 0.2 * spectrum_noise(1.6)
    if kind == "ground":
        return 0.5 * spectrum_noise(1.5) + 0.5 * bumps(900, 4)
    if kind == "plaster":
        return 1.0 * spectrum_noise(2.2) + 0.3 * spectrum_noise(1.5)
    if kind == "skin":
        # pores: larger dents, a fine wrinkle grain across them; nothing broader (it read as dirt on faces)
        return highpass(0.75 * bumps(1300, 3.4, -1) + 0.25 * spectrum_noise(1.0, (1.0, 3.0)), 8)
    raise ValueError(kind)


# --- detail colour maps (Detail Textures): a surface's fine grain in light and shade, mean grey, per kind of material.
# Two channels: r the grain tiled about 8 times the texture's own resolution, g a finer one tiled 4 times further
# (seen close up). The renderer's array holds them after the detail normal maps (index 7 on), in this order.
ALBEDO = ["stone", "brick", "concrete", "wood", "metal", "fabric", "ground", "roof", "foliage", "paper", "plaster",
          "skin", "snow", "grain"]


def cells(count):
    """tileable cells: distance to the nearest point, to the second, and a value per cell (-1..1)"""
    return grid_cells(count)


def specks(count, radius, bright=0.5):
    """sparse small flecks, lighter or darker (a mineral's, a pebble's, a fibre's end)"""
    f1, _, v = cells(count)
    return np.exp(-(f1 / radius) ** 2) * np.where(v > 1 - 2 * bright, 1.0, -1.0) * (0.5 + 0.5 * np.abs(v))


def fibres(count, length, width):
    """thin straight fibres at random angles (paper's)"""
    img = np.zeros((N, N), np.float32)
    yy, xx = np.mgrid[0:N, 0:N].astype(np.float32)
    for _ in range(count):
        cx, cy = rng.uniform(0, N, 2)
        a = rng.uniform(0, np.pi)
        ux, uy = np.cos(a), np.sin(a)
        dx = (xx - cx + N / 2) % N - N / 2
        dy = (yy - cy + N / 2) % N - N / 2
        along = dx * ux + dy * uy
        across = -dx * uy + dy * ux
        img += np.exp(-(across / width) ** 2) * np.clip(1 - np.abs(along) / length, 0, 1) * rng.choice([-1, 1])
    return img


def weave(cycles):
    y, x = np.mgrid[0:N, 0:N] * (2 * np.pi * cycles / N)
    warp = np.abs(np.sin(x)) * (np.sin(y / 2) > 0)
    weft = np.abs(np.sin(y)) * (np.sin(x / 2) <= 0)
    return norm(warp + weft - 0.5)


def peaks(x):
    """a sparse field's features as about two deviations at their strongest (normalising one made lone white dots)"""
    return x / (np.abs(x).max() + 1e-9) * 2.0


def albedo(kind):
    """the two grains (fields about a deviation) of a kind: a natural grain first, the kind's own features faint"""
    sn = spectrum_noise
    if kind == "stone":
        return (0.7 * sn(1.4) + 0.5 * sn(0.7) + 0.25 * peaks(specks(9000, 0.9, 0.5)),
                0.7 * sn(0.8) + 0.4 * sn(0.5) + 0.2 * peaks(specks(12000, 0.7, 0.5)))
    if kind == "brick":
        return (0.6 * sn(1.3) + 0.6 * sn(0.6) - 0.2 * peaks(bumps(9000, 0.9)),
                0.7 * sn(0.7) + 0.4 * sn(0.4))
    if kind == "concrete":
        f1, f2, v = cells(6000)
        aggregate = v * np.clip((f2 - f1) / 1.5, 0, 1)
        return (0.35 * norm(aggregate) + 0.5 * sn(1.3) + 0.5 * sn(0.6) - 0.15 * peaks(bumps(6000, 0.9)),
                0.6 * sn(0.7) + 0.4 * sn(0.4) + 0.15 * peaks(specks(12000, 0.7, 0.5)))
    if kind == "wood":
        return (0.9 * sn(1.7, (14.0, 0.25)) + 0.5 * sn(1.1, (30.0, 0.3)) + 0.2 * sn(0.8),
                0.9 * sn(1.1, (40.0, 0.35)) + 0.3 * sn(0.6))
    if kind == "metal":
        return (0.8 * sn(1.5, (0.06, 12.0)) + 0.4 * sn(0.7) - 0.15 * peaks(bumps(3000, 1.0)),
                0.8 * sn(1.0, (0.05, 18.0)) + 0.4 * sn(0.5))
    if kind == "fabric":
        return (0.5 * weave(64) + 0.5 * sn(1.3) + 0.3 * sn(0.7),
                0.5 * weave(128) + 0.5 * sn(0.7))
    if kind == "ground":
        f1, f2, v = cells(5000)
        pebbles = v * np.clip(1 - f1 / 2.2, 0, 1)
        return (0.5 * sn(1.6) + 0.5 * sn(0.7) + 0.35 * norm(pebbles),
                0.6 * sn(0.7) + 0.4 * sn(0.4) + 0.2 * peaks(specks(15000, 0.6, 0.5)))
    if kind == "roof":
        return (0.4 * peaks(bumps(12000, 0.9)) + 0.5 * sn(0.7) + 0.4 * sn(1.5),
                0.7 * sn(0.6) + 0.3 * sn(0.4))
    if kind == "foliage":
        f1, f2, _ = cells(1200)
        veins = -np.exp(-((f2 - f1) / 1.0) ** 2)
        return (0.3 * norm(veins) + 0.6 * sn(1.3) + 0.4 * sn(0.7), 0.7 * sn(0.7) + 0.2 * norm(veins))
    if kind == "paper":
        return (0.3 * norm(fibres(600, 14, 0.6)) + 0.6 * sn(1.5) + 0.4 * sn(0.7),
                0.25 * norm(fibres(900, 8, 0.5)) + 0.7 * sn(0.7))
    if kind == "plaster":
        return (0.7 * sn(2.0) + 0.4 * sn(0.8) - 0.12 * peaks(bumps(9000, 0.8)),
                0.7 * sn(0.8) + 0.4 * sn(0.5))
    if kind == "skin":
        return (0.5 * sn(1.3, (1.0, 2.5)) + 0.4 * sn(0.8) - 0.25 * peaks(bumps(12000, 0.8)),
                0.6 * sn(0.7) - 0.2 * peaks(bumps(15000, 0.6)))
    if kind == "snow":
        sparkle = np.clip(specks(9000, 0.7, 0.95), 0, None)
        return (0.6 * sn(1.7) + 0.4 * sn(0.8) + 0.3 * peaks(sparkle), 0.5 * sn(0.8) + 0.4 * peaks(sparkle))
    if kind == "grain":
        return (0.7 * sn(1.3) + 0.4 * sn(0.7), 0.8 * sn(0.7))
    raise ValueError(kind)


def grey(h):
    """a normalised field as a mean grey picture: three deviations either side"""
    return np.clip(0.5 + norm(h) * 0.16, 0, 1)


def normals(h, strength):
    """strength: the RMS slope, so every kind tilts about as much whatever its spectrum"""
    dx = np.roll(h, -1, 1) - np.roll(h, 1, 1)
    dy = np.roll(h, -1, 0) - np.roll(h, 1, 0)
    k = strength / (np.sqrt(np.mean(dx * dx + dy * dy)) + 1e-9)
    dx, dy = dx * k, dy * k
    n = np.dstack([-dx, dy, np.ones_like(h)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    return n * 0.5 + 0.5


def main():
    pack = sys.argv[1]
    out = os.path.join(pack, "details")
    work = os.path.join(pack, "work_details")
    os.makedirs(out, exist_ok=True)
    os.makedirs(work, exist_ok=True)
    files = []
    only = sys.argv[2:]  # names to make (others kept)
    for i, kind in enumerate(materials.DETAILS):
        if only and kind not in only:
            continue
        h = height(kind)
        path = os.path.join(work, "%d.png" % i)
        Image.fromarray((normals(h, 0.35) * 255 + 0.5).astype(np.uint8), "RGB").save(path)
        files.append(path)
        print(kind)
    # the colour grains after them (BC5 as the normals: one array): r the grain, g the finer one
    for j, kind in enumerate(ALBEDO):
        i = len(materials.DETAILS) + j
        if only and "albedo_" + kind not in only and "albedo" not in only:
            continue
        # (the broad shapes taken off: the grain adds fine detail and never stains the original's design)
        a, b = albedo(kind)
        a, b = highpass(a, 10), highpass(b, 6)
        img = np.dstack([grey(a), grey(b), np.zeros((N, N))])
        path = os.path.join(work, "%d.png" % i)
        Image.fromarray((img * 255 + 0.5).astype(np.uint8), "RGB").save(path)
        files.append(path)
        print("albedo", kind, i)
    generate.texconv(files, "BC5_UNORM", out)


if __name__ == "__main__":
    main()
