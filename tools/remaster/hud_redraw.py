"""The HUD's frames and the player's posture drawn again from scratch, as vectors, at 8 times the original's size: the
ammunition's box (its round and its three magazines), the health bar's frame (its cross), the inventory's box (its
bracket) and the three postures (standing, crouching, lying). The frames' straight edges are the original's own
pixels at the new size (they are straight lines: exact), every curved or slanted part a shape drawn anew, antialiased.
The postures' silhouettes are traced smooth from the originals (their outline light, their inside faint), the same
size and place in their boxes, so the game's quads show them where they were. Written to the pack as BC7 with mips.

    python -I hud_redraw.py <extract folder> <pack folder>
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate  # noqa: E402

K = 8    # the new size, times the original's
SS = 4   # supersampling of the drawn shapes

AMMO, HEALTH, INVENTORY, MEDKIT = "f62d1fded3b3cc9a", "7c691e4b5741194e", "55611bd9a3025440", "0b41acaa2b79af57"
POSTURES = {"crouch": "78aa5a7bf1a08670", "prone": "a4db31f92c1ea1a4", "stand": "e67bb8cbd91d5674"}


def nearest(src):
    """the original at K times, each pixel a square: its frames' straight edges as they are"""
    h, w = src.shape[:2]
    return np.asarray(Image.fromarray(src.astype(np.uint8), "RGBA").resize((w * K, h * K), Image.NEAREST)).astype(np.float32)


class Canvas:
    """shapes in the original's pixel coordinates, drawn antialiased into an RGBA layer at K times"""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.cov = Image.new("L", (w * K * SS, h * K * SS), 0)
        self.d = ImageDraw.Draw(self.cov)

    def pts(self, ps):
        return [(x * K * SS, y * K * SS) for x, y in ps]

    def poly(self, ps, fill=255):
        self.d.polygon(self.pts(ps), fill=fill)

    def rect(self, x0, y0, x1, y1, fill=255, r=0):
        self.d.rounded_rectangle(self.pts([(x0, y0), (x1, y1)]), radius=r * K * SS, fill=fill)

    def coverage(self):
        return np.asarray(self.cov.resize((self.w * K, self.h * K), Image.BOX)).astype(np.float32) / 255


def over(base, cov, rgb, alpha):
    """a flat colour laid over base where cov (0..1) covers, straight alpha"""
    a = cov * alpha
    out = base.copy()
    ba = base[..., 3:] / 255
    na = a[..., None] / 255 + ba * (1 - a[..., None] / 255)
    for c in range(3):
        out[..., c] = (rgb[c] * a / 255 + base[..., c] * ba[..., 0] * (1 - a / 255)) / np.maximum(na[..., 0], 1e-6)
    out[..., 3] = na[..., 0] * 255
    return out


def fill(base, x0, y0, x1, y1, rgba):
    base[y0 * K:y1 * K, x0 * K:x1 * K] = rgba
    return base


def ammo(src):
    out = nearest(src)
    # the round and the magazines' place cleared to the box's own inside
    out = fill(out, 2, 6, 38, 26, (0, 0, 0, 85))
    c = Canvas(64, 32)
    # a rifle round: the case, its rim, the bullet's ogive to a point
    c.rect(3.0, 11.4, 6.4, 24.6, r=0.35)
    c.rect(2.7, 23.6, 6.7, 24.9, r=0.25)
    c.poly([(3.25, 11.6), (3.6, 9.4), (4.7, 6.9), (5.8, 9.4), (6.15, 11.6)])
    out = over(out, c.coverage(), (196, 196, 196), 211)
    g = Canvas(64, 32)  # the case's neck and the line where the bullet meets it, a little darker
    g.rect(3.0, 11.3, 6.4, 11.9)
    g.rect(4.4, 13.5, 5.0, 22.8, r=0.3)
    out = over(out, g.coverage(), (120, 120, 120), 160)
    # three magazines, slanted as the original's (each row a pixel further left), their windows dark
    for x, y in ((28.0, 8.0), (27.0, 14.0), (26.0, 20.0)):
        m = Canvas(64, 32)
        sl = 1.0  # slant over the magazine's height
        m.poly([(x + sl, y), (x + sl + 7.6, y), (x + 7.6, y + 4.0), (x, y + 4.0)])
        out = over(out, m.coverage(), (196, 196, 196), 211)
        h = Canvas(64, 32)
        h.poly([(x + sl + 1.6, y + 1.1), (x + sl + 5.9, y + 1.1), (x + 6.1, y + 2.9), (x + 1.8, y + 2.9)])
        out = over(out, h.coverage(), (40, 40, 40), 170)
    return out


def health(src):
    # all straight lines: the original's pixels at the new size are its vector drawing
    return nearest(src)


def inventory(src):
    out = nearest(src)
    out[:, : 12 * K] = (255, 255, 255, 0)
    c = Canvas(64, 64)
    # the bracket on the box's left: a crescent bowing out, its ends on the box's edge
    n = 48
    top, bot, mid = 15.0, 49.0, 32.0
    outer = [(11.9 - 9.6 * np.sin(np.pi * t / n), top + (bot - top) * t / n) for t in range(n + 1)]
    inner = [(11.9 - 6.0 * np.sin(np.pi * t / n), top + 1.8 + (bot - top - 3.6) * t / n) for t in range(n, -1, -1)]
    c.poly(outer + inner)
    out = over(out, c.coverage(), (153, 153, 153), 153)
    e = Canvas(64, 64)  # its outer rim brighter
    rim = [(11.9 - 9.6 * np.sin(np.pi * t / n), top + (bot - top) * t / n) for t in range(n + 1)]
    rim2 = [(11.9 - 8.4 * np.sin(np.pi * t / n), top + 0.9 + (bot - top - 1.8) * t / n) for t in range(n, -1, -1)]
    e.poly(rim + rim2)
    out = over(out, e.coverage(), (200, 200, 200), 170)
    return out


def blur(a, r):
    """separable gaussian (PIL blurs no float images)"""
    n = max(1, int(r * 3))
    x = np.arange(-n, n + 1, dtype=np.float32)
    k = np.exp(-x * x / (2 * r * r))
    k /= k.sum()
    p = np.pad(a, ((0, 0), (n, n)), mode="edge")
    a = sum(k[i] * p[:, i:i + a.shape[1]] for i in range(len(k)))
    p = np.pad(a, ((n, n), (0, 0)), mode="edge")
    return sum(k[i] * p[i:i + a.shape[0], :] for i in range(len(k)))


def contours(f, level):
    """marching squares: the closed loops where f crosses level, in f's pixel coordinates"""
    h, w = f.shape
    f = np.pad(f, 1, constant_values=f.min() - 1)
    segs = {}

    def interp(y0, x0, y1, x1):
        a, b = f[y0, x0], f[y1, x1]
        t = (level - a) / (b - a) if b != a else 0.5
        return (x0 + (x1 - x0) * t - 1, y0 + (y1 - y0) * t - 1)

    out = []
    for y in range(h + 1):
        for x in range(w + 1):
            c = [f[y, x] > level, f[y, x + 1] > level, f[y + 1, x + 1] > level, f[y + 1, x] > level]
            idx = c[0] * 8 + c[1] * 4 + c[2] * 2 + c[3]
            if idx in (0, 15):
                continue
            e = {0: lambda: interp(y, x, y, x + 1), 1: lambda: interp(y, x + 1, y + 1, x + 1),
                 2: lambda: interp(y + 1, x, y + 1, x + 1), 3: lambda: interp(y, x, y + 1, x)}
            table = {1: [(3, 2)], 2: [(2, 1)], 3: [(3, 1)], 4: [(0, 1)], 5: [(3, 0), (2, 1)], 6: [(0, 2)], 7: [(3, 0)],
                     8: [(0, 3)], 9: [(0, 2)], 10: [(0, 1), (2, 3)], 11: [(0, 1)], 12: [(1, 3)], 13: [(1, 2)], 14: [(2, 3)]}
            for a, b in table[idx]:
                out.append((e[a](), e[b]()))
    # link the segments into loops (undirected: each point joins two segments)
    def key(p):
        return (round(p[0], 4), round(p[1], 4))
    adj, pos = {}, {}
    for n, (a, b) in enumerate(out):
        ka, kb = key(a), key(b)
        pos[ka], pos[kb] = a, b
        adj.setdefault(ka, []).append((kb, n))
        adj.setdefault(kb, []).append((ka, n))
    loops, used = [], set()
    for n, (a, b) in enumerate(out):
        if n in used:
            continue
        used.add(n)
        start, prev, cur = key(a), key(a), key(b)
        loop = [pos[start], pos[cur]]
        while cur != start:
            step = [(k, m) for k, m in adj.get(cur, []) if m not in used]
            if not step:
                break
            k, m = step[0]
            used.add(m)
            prev, cur = cur, k
            loop.append(pos[cur])
        if len(loop) > 8:
            loops.append(np.array(loop))
    return loops


def smooth_loop(p, n):
    """a closed loop's points averaged with their n neighbours each side, twice: the pixel steps gone"""
    for _ in range(2):
        q = np.zeros_like(p)
        for k in range(-n, n + 1):
            q += np.roll(p, k, axis=0)
        p = q / (2 * n + 1)
    return p


def posture(src):
    """the soldier drawn anew: his silhouette traced from the original and smoothed into curves, a faint grey body
    with a light outline and a soft dark halo, at the original's place (its levels: 118 at 122 inside, 205 on the
    line, 35 about it)"""
    h, w = src.shape[:2]
    a = src[..., 3] / 255.0
    U = 4  # the tracing grid, times the original
    up = np.asarray(Image.fromarray(a.astype(np.float32), "F").resize((w * U, h * U), Image.BICUBIC))
    up = blur(up, U * 0.35)
    loops = [smooth_loop(l / U, 3) for l in contours(up, 0.33)]
    W, H = w * K, h * K
    S = K * SS
    fill_img = Image.new("L", (W * SS, H * SS), 0)
    line_img = Image.new("L", (W * SS, H * SS), 0)
    for l in loops:  # even-odd: a loop inside another is a hole (between the legs)
        m = Image.new("L", fill_img.size, 0)
        ImageDraw.Draw(m).polygon([(x * S, y * S) for x, y in l], fill=255)
        fill_img = Image.fromarray(np.asarray(fill_img) ^ np.asarray(m))
        pts = [(x * S, y * S) for x, y in l] + [(l[0][0] * S, l[0][1] * S)]
        ImageDraw.Draw(line_img).line(pts, fill=255, width=int(S * 0.85), joint="curve")
    body = np.asarray(fill_img.resize((W, H), Image.BOX)).astype(np.float32) / 255
    line = np.asarray(line_img.resize((W, H), Image.BOX)).astype(np.float32) / 255
    halo = np.clip(blur(body, K * 0.9) * 1.4, 0, 1)
    out = np.zeros((H, W, 4), np.float32)
    out = over(out, halo, (35, 35, 35), 50)
    out = over(out, body, (118, 118, 118), 122)
    out = over(out, line, (205, 205, 205), 215)
    return out


def noise(W, H, scale, seed):
    """smooth value noise 0..1 at a cell of scale new pixels"""
    rng = np.random.default_rng(seed)
    g = rng.random((H // scale + 2, W // scale + 2)).astype(np.float32)
    return np.asarray(Image.fromarray(g, "F").resize((W + 2 * scale, H + 2 * scale), Image.BICUBIC))[scale:scale + H, scale:scale + W]


def medkit(src):
    """the medical pouch drawn anew: khaki canvas, its flap with a stitched edge and a strap, a cream disc with a red
    cross, a soft shadow under it (the original's place and size: 11..50 by 11..47 of 64)"""
    W, H = 64 * K, 64 * K
    out = np.zeros((H, W, 4), np.float32)
    body = Canvas(64, 64)
    body.rect(11.2, 11.5, 50.3, 47.2, r=3.2)
    b = body.coverage()
    shadow = np.roll(np.roll(blur(b, K * 1.6), int(1.5 * K), 0), int(0.8 * K), 1)
    out = over(out, np.clip(shadow, 0, 1), (0, 0, 0), 130)
    # the canvas: light at the top, darker towards the bottom, its weave a fine noise
    y = np.linspace(0, 1, H)[:, None] * np.ones((1, W))
    t = np.clip((y * 64 - 11) / 36, 0, 1)
    weave = noise(W, H, 3, 1) * 0.6 + noise(W, H, 11, 2) * 0.4
    base = np.stack([196 - 80 * t, 181 - 82 * t, 140 - 75 * t], -1) * (0.88 + 0.2 * weave[..., None])
    canvas = np.zeros((H, W, 4), np.float32)
    canvas[..., :3] = base
    canvas[..., 3] = b * 255
    out = canvas_over(out, canvas)
    # the pocket below the flap in shade
    pocket = Canvas(64, 64)
    pocket.rect(11.2, 33.5, 50.3, 47.2, r=3.0)
    out = over(out, pocket.coverage() * b, (40, 30, 12), 95)
    # the flap's edge: a dark seam under it, a light rim on it
    seam = Canvas(64, 64)
    seam.poly([(11.2, 32.2), (50.3, 32.2), (50.3, 34.0), (11.2, 34.0)])
    out = over(out, seam.coverage() * b, (35, 26, 8), 170)
    rim = Canvas(64, 64)
    rim.poly([(11.6, 31.2), (49.9, 31.2), (49.9, 32.2), (11.6, 32.2)])
    out = over(out, rim.coverage(), (214, 200, 160), 120)
    # stitches along the flap and the pouch's edges
    st = Canvas(64, 64)
    for x in np.arange(13.5, 48.5, 2.2):
        st.rect(x, 29.8, x + 1.1, 30.3)
        st.rect(x, 45.3, x + 1.1, 45.8)
    for yy in np.arange(14.0, 44.0, 2.2):
        st.rect(13.0, yy, 13.5, yy + 1.1)
        st.rect(48.0, yy, 48.5, yy + 1.1)
    out = over(out, st.coverage(), (90, 72, 38), 150)
    # the strap and its buckle
    strap = Canvas(64, 64)
    strap.rect(28.9, 26.5, 32.5, 38.5, r=0.6)
    out = over(out, strap.coverage(), (120, 98, 58), 230)
    buckle = Canvas(64, 64)
    buckle.rect(28.4, 35.4, 33.0, 38.2, r=0.5)
    out = over(out, buckle.coverage(), (70, 58, 34), 230)
    hole = Canvas(64, 64)
    hole.rect(29.6, 36.2, 31.8, 37.4, r=0.3)
    out = over(out, hole.coverage(), (150, 130, 90), 230)
    # the disc and its cross
    disc = Canvas(64, 64)
    disc.d.ellipse(disc.pts([(24.0, 14.0), (37.4, 27.4)]), fill=255)
    out = over(out, disc.coverage(), (244, 236, 205), 255)
    ring = Canvas(64, 64)
    ring.d.ellipse(ring.pts([(24.0, 14.0), (37.4, 27.4)]), outline=255, width=int(0.7 * K * SS))
    out = over(out, ring.coverage(), (170, 150, 105), 200)
    cross = Canvas(64, 64)
    cross.rect(29.0, 16.4, 32.4, 25.0, r=0.4)
    cross.rect(26.4, 19.0, 35.0, 22.4, r=0.4)
    out = over(out, cross.coverage(), (180, 98, 62), 255)
    return out


def canvas_over(base, layer):
    a = layer[..., 3:] / 255
    ba = base[..., 3:] / 255
    na = a + ba * (1 - a)
    out = base.copy()
    out[..., :3] = (layer[..., :3] * a + base[..., :3] * ba * (1 - a)) / np.maximum(na, 1e-6)
    out[..., 3:] = na * 255
    return out


def main():
    src_dir, pack = sys.argv[1], sys.argv[2]
    work = os.path.join(pack, "work_hud")
    os.makedirs(work, exist_ok=True)
    jobs = {AMMO: ammo, HEALTH: health, INVENTORY: inventory, MEDKIT: medkit}
    jobs.update({rid: posture for rid in POSTURES.values()})
    files = []
    for rid, fn in jobs.items():
        src = np.asarray(Image.open(os.path.join(src_dir, rid + ".png")).convert("RGBA")).astype(np.float32)
        out = fn(src)
        p = os.path.join(work, rid + ".c.png")
        Image.fromarray(np.clip(out, 0, 255).astype(np.uint8), "RGBA").save(p)
        files.append(p)
        print(rid, out.shape[1], "x", out.shape[0])
    generate.texconv(files, "BC7_UNORM", os.path.join(pack, "textures"))


if __name__ == "__main__":
    main()
