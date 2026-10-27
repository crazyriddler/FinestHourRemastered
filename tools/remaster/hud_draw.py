"""The in-game HUD's icons drawn from scratch, each a small vector drawing of what it shows (as the medkit and the
T34 of hud_redraw.py / hud_redraw2.py are), in the original's place, size, colours and opacities: none of the
original's pixels kept, nothing traced. Shapes in the original's pixel coordinates, drawn antialiased at K times.

    python -I hud_draw.py <extract folder> <pack folder> [names,...]

Writes <pack>/work_hud3/<id>.c.png and the pack's BC7 textures (after hud_redraw2.py, whose icons it replaces).
"""
import json
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate  # noqa: E402
from hud_redraw import K, SS, blur  # noqa: E402

S = K * SS


# --- drawing -------------------------------------------------------------------------------------------------------

class Shape:
    """a coverage mask built from shapes (add) and holes (cut), in the original's coordinates"""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.img = Image.new("L", (w * S, h * S), 0)
        self.d = ImageDraw.Draw(self.img)

    @staticmethod
    def P(pts):
        return [(x * S, y * S) for x, y in pts]

    def poly(self, pts, on=True):
        self.d.polygon(self.P(pts), fill=255 if on else 0)
        return self

    def rect(self, x0, y0, x1, y1, r=0, on=True):
        self.d.rounded_rectangle(self.P([(x0, y0), (x1, y1)]), radius=r * S, fill=255 if on else 0)
        return self

    def ellipse(self, cx, cy, rx, ry=None, on=True):
        ry = rx if ry is None else ry
        self.d.ellipse(self.P([(cx - rx, cy - ry), (cx + rx, cy + ry)]), fill=255 if on else 0)
        return self

    def line(self, pts, width, on=True, caps=True):
        """a thick polyline, its joints and ends round"""
        f = 255 if on else 0
        self.d.line(self.P(pts), fill=f, width=max(1, int(width * S)), joint="curve")
        if caps:
            for x, y in (pts[0], pts[-1]):
                self.d.ellipse(self.P([(x - width / 2, y - width / 2), (x + width / 2, y + width / 2)]), fill=f)
        return self

    def ring(self, cx, cy, rx, ry, width, on=True):
        self.ellipse(cx, cy, rx + width / 2, ry + width / 2, on)
        self.ellipse(cx, cy, rx - width / 2, ry - width / 2, not on)
        return self

    def arc(self, cx, cy, rx, ry, a0, a1, width, on=True, n=48):
        pts = [(cx + rx * math.cos(math.radians(a)), cy + ry * math.sin(math.radians(a))) for a in np.linspace(a0, a1, n)]
        return self.line(pts, width, on)

    def cov(self):
        return np.asarray(self.img.resize((self.w * K, self.h * K), Image.BOX)).astype(np.float32) / 255


def erode(cov, r):
    """cov shrunk by r pixels (of the K times image), an octagon's reach"""
    if r < 1:
        return cov
    r = int(round(r))
    out = cov
    for axis in (0, 1):  # a square's
        p = np.pad(out, [(r, r) if i == axis else (0, 0) for i in range(2)], mode="constant")
        n = out.shape[axis]
        out = np.min([np.take(p, range(k, k + n), axis=axis) for k in range(2 * r + 1)], axis=0)
    d = int(round(r * 0.7))  # and a diamond's, the corners cut
    p = np.pad(cov, d, mode="constant")
    h, w = cov.shape
    dia = np.min([p[d + dy:d + dy + h, d + dx:d + dx + w] for dy in range(-d, d + 1) for dx in range(-d, d + 1)
                  if abs(dx) + abs(dy) <= d], axis=0) if d else cov
    return np.minimum(out, dia)


def outline(cov, width):
    """the band of width (the original's pixels) inside cov's edge"""
    return np.clip(cov - erode(cov, width * K), 0, 1)


class Pic:
    """layers of colour over each other: (coverage, colour or a colour per pixel, opacity 0..1)"""

    def __init__(self, w=64, h=64):
        self.w, self.h = w, h
        self.out = np.zeros((h * K, w * K, 4), np.float32)

    def shape(self):
        return Shape(self.w, self.h)

    def lay(self, cov, rgb, alpha):
        if isinstance(cov, Shape):
            cov = cov.cov()
        a2 = np.clip(cov * alpha, 0, 1)
        oa = self.out[..., 3] / 255
        na = a2 + oa * (1 - a2)
        rgb = np.asarray(rgb, np.float32)
        if rgb.ndim == 1:
            rgb = rgb[None, None, :]
        self.out[..., :3] = (rgb * a2[..., None] + self.out[..., :3] * (oa * (1 - a2))[..., None]) / np.maximum(na, 1e-6)[..., None]
        self.out[..., 3] = na * 255
        return self

    def grid(self):
        """each pixel's middle in the original's coordinates (x, y)"""
        yy, xx = np.mgrid[0:self.h * K, 0:self.w * K].astype(np.float32)
        return (xx + 0.5) / K, (yy + 0.5) / K


def shade(base, k):
    return np.clip(np.asarray(base, np.float32) * k, 0, 255)


# --- the action icons: a green pictogram in a frame, three tones (line, half, faint) ------------------------------

GREEN = (153, 204, 102)
DARK = (20, 26, 20)
LINE, HALF, FAINT = 0.94, 0.64, 0.30


def frame_big(p):
    s = p.shape().rect(4, 1, 60, 63, r=4.5)
    s.rect(7, 4, 57, 60, r=2.2, on=False)
    p.lay(s, GREEN, LINE)


def frame_small(p):
    s = p.shape().rect(8, 5, 56, 59, r=8)
    s.rect(10.2, 7.2, 53.8, 56.8, r=6.2, on=False)
    p.lay(s, GREEN, LINE)


def mine(p, cy, rays):
    """an anti-tank mine seen from a little above: its body, its pressure plate's rings, sparks over it"""
    body = p.shape().ellipse(32, cy + 7, 24, 6.5).rect(8, cy, 56, cy + 7).ellipse(32, cy, 24, 6.5)
    p.lay(body, GREEN, LINE)
    top = p.shape().ellipse(32, cy, 21.5, 5.2)
    p.lay(top, DARK, 0.42)
    side = p.shape().rect(8, cy + 1.5, 56, cy + 7).ellipse(32, cy + 7, 24, 6.5)
    side.ellipse(32, cy, 24, 6.5, on=False)
    p.lay(side, DARK, 0.12)
    rings = p.shape().ring(32, cy, 14, 3.4, 0.9).ring(32, cy, 6, 1.5, 1.1)
    p.lay(rings, GREEN, LINE)
    dots = p.shape()
    for a in range(0, 360, 30):
        dots.ellipse(32 + 17.8 * math.cos(math.radians(a)), cy + 4.3 * math.sin(math.radians(a)), 0.7, 0.45)
    p.lay(dots, GREEN, LINE)
    if rays:
        r = p.shape()
        for a in (-160, -125, -90, -55, -20):
            ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
            r.line([(32 + 16 * ca, cy - 4 + 6 * sa), (32 + 24 * ca, cy - 4 + 10 * sa)], 1.3)
        p.lay(r, GREEN, LINE)


def arrow(p, x, y0, y1, shaft, head, head_len):
    """a straight arrow from y0 to y1 (its point at y1)"""
    s = p.shape()
    d = 1 if y1 > y0 else -1
    hy = y1 - d * head_len
    s.rect(x - shaft / 2, min(y0, hy), x + shaft / 2, max(y0, hy))
    s.poly([(x - head / 2, hy), (x + head / 2, hy), (x, y1)])
    p.lay(s, GREEN, LINE)


def armmine(src):
    p = Pic()
    frame_big(p)
    mine(p, 21, True)
    arrow(p, 32, 37, 58, 7, 22, 10)
    return p


def disarmmine(src):
    p = Pic()
    frame_big(p)
    arrow(p, 32, 25, 5, 7, 20, 9)
    mine(p, 37, False)
    return p


def dismount(src):
    p = Pic()
    frame_big(p)
    t = p.shape()
    t.rect(25, 5.5, 43, 11.5, r=2.2)            # turret
    t.rect(8, 7.2, 27, 9.4, r=0.8)               # gun
    t.poly([(12, 11), (54, 11), (57, 15), (9, 15)])  # hull
    t.rect(8, 14, 57, 26, r=6)                   # tracks
    p.lay(t, GREEN, LINE)
    w = p.shape()
    for x in (15, 23.5, 32.5, 41.5, 50):
        w.ellipse(x, 20, 3.3, 3.3)
    p.lay(w, GREEN, 0)  # (holes: the wheels see through)
    holes = p.shape()
    for x in (15, 23.5, 32.5, 41.5, 50):
        holes.ellipse(x, 20, 3.3, 3.3)
    p.out[..., 3] *= 1 - holes.cov()
    hubs = p.shape()
    for x in (15, 23.5, 32.5, 41.5, 50):
        hubs.ellipse(x, 20, 1.1, 1.1)
    p.lay(hubs, GREEN, HALF)
    arrow(p, 32, 29, 59, 12, 40, 19)
    return p


def drivevehicle(src):
    p = Pic()
    frame_big(p)
    wheel = p.shape().ring(32, 31, 21, 9.5, 2.6)
    wheel.line([(13, 33), (25, 36)], 2.4).line([(51, 33), (39, 36)], 2.4).line([(32, 36), (32, 41)], 2.6)
    wheel.rect(25, 33, 39, 39, r=2.5)
    p.lay(wheel, GREEN, LINE)
    hub = p.shape().rect(28, 34.5, 36, 37.5, r=1.2)
    p.lay(hub, DARK, 0.85)
    for flip in (False, True):
        X = (lambda x: 64 - x) if flip else (lambda x: x)
        # the forearm in its sleeve from the bottom corner, the gloved fist round the rim at ten or two
        arm = p.shape().line([(X(8), 64), (X(14), 36)], 11, caps=False).line([(X(14), 36), (X(15), 32)], 10)
        ac = arm.cov()
        p.lay(ac, DARK, 0.9)
        p.lay(ac, GREEN, FAINT + 0.1)
        p.lay(outline(ac, 1.0), GREEN, LINE)
        fist = p.shape().line([(X(9.5), 30), (X(20), 22.5)], 9.5)
        fc = fist.cov()
        p.lay(fc, DARK, 0.9)
        p.lay(fc, GREEN, HALF)
        p.lay(outline(fc, 1.0), GREEN, LINE)
        k = p.shape()
        for t in (0.3, 0.55, 0.8):  # the fingers' gaps across the fist
            x0, y0 = 9.5 + 10.5 * t, 30 - 7.5 * t
            k.line([(X(x0 - 2.2), y0 - 3.0), (X(x0 + 0.4), y0 + 0.6)], 0.8)
        p.lay(k, GREEN, LINE)
    return p


def hand(p, flip=False):
    """an open hand, fingers up, its cuff at the bottom"""
    X = (lambda x: x) if not flip else (lambda x: 64 - x)
    h = p.shape()
    h.rect(X(19) if not flip else X(45), 30, X(45) if not flip else X(19), 51, r=7)
    for cx, top, wid in ((23, 13, 5.0), (29, 8.5, 5.2), (35, 9.5, 5.2), (41, 14.5, 4.8)):
        h.line([(X(cx), top + wid / 2), (X(cx), 36)], wid)
    h.line([(X(20), 44), (X(13), 33), (X(11.5), 28)], 5.2)
    h.rect(X(24) if not flip else X(41), 48, X(41) if not flip else X(24), 57)
    hc = h.cov()
    p.lay(hc, GREEN, HALF)
    p.lay(outline(hc, 1.2), GREEN, LINE)
    cuff = p.shape().ellipse(X(32.5), 60.5, 15, 6)
    cuff.rect(0, 60, 64, 64, on=False)
    p.lay(cuff, GREEN, LINE)
    creases = p.shape()
    for cx in (23, 29, 35, 41):
        creases.line([(X(cx), 29), (X(cx), 31)], 0.8)
    p.lay(creases, GREEN, LINE * 0.8)


def get(src):
    p = Pic()
    frame_big(p)
    hand(p)
    return p


def healfriendly(src):
    p = Pic()
    frame_big(p)
    s = p.shape().poly([(8, 27), (32, 6), (56, 27), (50, 27), (50, 57), (14, 57), (14, 27)])
    s.rect(27, 31, 37, 52, r=0.6, on=False)
    s.rect(20, 36.5, 44, 46.5, r=0.6, on=False)
    p.lay(s, GREEN, LINE)
    return p


def opendoor(src):
    p = Pic()
    frame_big(p)
    door = p.shape().poly([(49, 9), (56.5, 4.5), (56.5, 63.0), (49, 58)])
    p.lay(door, GREEN, HALF)
    p.lay(outline(door.cov(), 1.0), GREEN, LINE)
    frame = p.shape().rect(25, 9, 51, 59)
    frame.rect(28, 12, 48, 59, on=False)
    p.lay(frame, GREEN, LINE)
    floor = p.shape().poly([(25, 58), (49, 58), (56.5, 62.4), (25, 62.4)])
    p.lay(floor, GREEN, FAINT)
    knob = p.shape().ellipse(52.3, 35, 1.3, 1.6)
    p.lay(knob, (20, 26, 20), 0.9)
    a = p.shape()
    a.rect(8, 31.5, 33, 38.5)
    a.poly([(32, 25), (44, 35), (32, 45)])
    p.lay(a, GREEN, LINE)
    return p


def pickupweapon(src):
    p = Pic()
    frame_big(p)
    # a rifle's stock and butt at the bottom right, a hand reaching down for it, palm down, from the top
    stock = p.shape().poly([(34, 59.5), (56.5, 37), (56.5, 59.5)])
    p.lay(stock, GREEN, HALF)
    p.lay(outline(stock.cov(), 1.1), GREEN, LINE)
    clip = p.shape().rect(7, 4, 57, 60, r=2.2)
    h = p.shape()
    h.line([(14, 2), (21, 17)], 11.5, caps=False)                     # the wrist from the top
    h.poly([(13, 15), (31, 13), (39, 27), (35, 37), (17, 39), (11, 28)])  # the back of the hand
    for (x0, y0), (x1, y1), wd in (((16.5, 35), (14, 48), 5.6), ((22.5, 37), (22.5, 51), 5.8),
                                    ((28.5, 36.5), (31.5, 49.5), 5.6), ((33.5, 33.5), (39.5, 44), 5.2)):
        h.line([(x0, y0), (x1, y1)], wd)
    h.line([(35, 22), (43, 21.5), (47.5, 26.5)], 5.6)                  # the thumb
    hc = h.cov() * clip.cov()
    p.lay(hc, DARK, 0.9)
    p.lay(outline(hc, 1.3), GREEN, LINE)
    knuckles = p.shape()
    for x0, y0 in ((17.5, 34), (23, 35.5), (28.8, 35), (33.5, 31.5)):
        knuckles.line([(x0 - 1.5, y0), (x0 + 1.5, y0 + 0.4)], 0.9)
    p.lay(knuckles, GREEN, LINE * 0.8)
    return p


def plantbomb(src):
    p = Pic()
    frame_big(p)
    loop = p.shape().ring(27, 13, 6.2, 7, 1.9)
    p.lay(loop, GREEN, LINE)
    body = p.shape().rect(24, 20, 40, 61, r=2.5)
    body.rect(30, 17, 37, 21, r=1)
    p.lay(body, GREEN, LINE)
    face = p.shape().ellipse(32, 40, 7.6)
    p.lay(face, (20, 26, 20), 0.88)
    ticks = p.shape()
    for a in range(0, 360, 30):
        ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
        ticks.line([(32 + 5.4 * ca, 40 + 5.4 * sa), (32 + 6.7 * ca, 40 + 6.7 * sa)], 0.8)
    ticks.line([(32, 40), (32, 35)], 1.0).line([(32, 40), (35.2, 41.8)], 1.0)
    p.lay(ticks, GREEN, LINE)
    straps = p.shape().rect(24, 27, 40, 28.3).rect(24, 52, 40, 53.3)
    p.lay(straps, (20, 26, 20), 0.6)
    return p


def radio(src):
    p = Pic()
    frame_big(p)
    ant = p.shape().line([(14, 4.5), (14, 31)], 1.6).rect(11.5, 27, 16.5, 32, r=1)
    p.lay(ant, GREEN, LINE)
    rays = p.shape()
    for a in (-150, -120, -90, -60, -30):
        ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
        rays.line([(41 + 9 * ca, 25 + 9 * sa), (41 + 17 * ca, 25 + 15 * sa)], 1.6)
    p.lay(rays, GREEN, LINE)
    body = p.shape().rect(8, 31, 56, 58, r=2.2)
    body.rect(24, 27.5, 40, 31.5, r=1.2)
    p.lay(body, GREEN, LINE)
    dk = (20, 26, 20)
    d = p.shape().ellipse(28, 44, 5.2).rect(43, 37, 51, 42, r=0.8).ellipse(47, 52, 1.8).rect(12, 48, 15, 54, r=1)
    p.lay(d, dk, 0.82)
    k = p.shape().ellipse(28, 44, 3.0)
    p.lay(k, GREEN, HALF)
    seam = p.shape().rect(8, 34, 56, 35)
    p.lay(seam, dk, 0.45)
    return p


def bomb(src):
    p = Pic()
    frame_small(p)
    xx, yy = p.grid()
    ball = p.shape().ellipse(29, 38, 13.5)
    neck = p.shape().poly([(36, 25), (41, 22), (44, 27), (39, 30)])
    bc = np.maximum(ball.cov(), neck.cov())
    # its shading: lit from the top left, a highlight, darker to the lower right
    d = np.hypot(xx - 24.5, yy - 33.5) / 20
    tone = np.clip(1.25 - d, 0.45, 1.25)[..., None]
    rgb = np.clip(np.asarray(GREEN, np.float32) * tone * 0.8, 0, 255)
    p.lay(bc, rgb, LINE)
    hl = p.shape().ellipse(23.5, 32, 3.0, 2.4)
    p.lay(blur(hl.cov(), K * 0.8), (225, 245, 200), 0.55)
    fuse = p.shape().line([(42, 24), (44, 19), (47.5, 16)], 1.2)
    p.lay(fuse, GREEN, LINE)
    sp = p.shape()
    for a in (-120, -80, -40, 0, 40):
        ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
        sp.line([(48 + 2.2 * ca, 15 + 2.2 * sa), (48 + 5.4 * ca, 15 + 5.4 * sa)], 0.9)
    p.lay(sp, GREEN, LINE)
    return p


def defuse(src):
    p = Pic()
    frame_small(p)
    s = p.shape()
    s.line([(38, 26), (30, 37), (21.5, 46.5), (16, 50)], 5.4)   # a handle
    s.line([(38, 26), (37.5, 38), (34, 47.5), (31.5, 53)], 5.4)  # the other
    s.poly([(35.5, 23), (42.5, 12.5), (46, 11.5), (41.5, 26)])   # the jaws, a little open
    s.poly([(38.5, 28.5), (49, 18.5), (51.5, 20), (41.5, 29.5)])
    s.ellipse(38.5, 26, 4.2)
    sc = s.cov()
    p.lay(sc, GREEN, HALF)
    p.lay(outline(sc, 1.0), GREEN, LINE)
    piv = p.shape().ellipse(38.5, 26, 1.6)
    p.lay(piv, DARK, 0.85)
    grip = p.shape()
    for t in np.linspace(0.15, 0.85, 5):  # the handles' grips
        ax, ay = 30 + (16 - 30) * t, 37 + (50 - 37) * t
        grip.line([(ax - 1.4, ay - 1.6), (ax + 1.4, ay + 1.6)], 0.7)
        bx, by = 37.5 + (31.5 - 37.5) * t, 38 + (53 - 38) * t
        grip.line([(bx - 1.9, by - 0.6), (bx + 1.9, by + 0.6)], 0.7)
    p.lay(grip, DARK, 0.5)
    return p


def soldier_running(s):
    """a soldier running to the left, his rifle held across: one silhouette"""
    s.ellipse(35.5, 17.2, 2.9, 3.1)                                    # head
    s.ellipse(35.6, 15.4, 4.4, 3.3)                                    # helmet, a dome
    s.rect(30.6, 15.6, 40.6, 17.0, r=0.6)                              # its rim
    s.poly([(30.5, 20.5), (39.5, 19.5), (42, 26), (40, 34.5), (30.5, 35), (28.5, 27)])  # torso, the pack behind
    s.rect(38, 21, 43.5, 29, r=1.5)
    s.line([(32.5, 34), (27.5, 41), (22, 46)], 5.0)                     # the leg ahead, bent
    s.line([(22, 46), (17, 49.5)], 3.6)
    s.line([(38, 34), (42.5, 42), (48, 47)], 5.0)                       # the leg behind
    s.line([(48, 47), (52.5, 45.5)], 3.4)
    s.line([(31.5, 22.5), (27, 28), (23, 27.5)], 3.4)                   # arms forward to the rifle
    s.line([(37, 22.5), (33.5, 29.5), (28, 29.5)], 3.2)
    s.line([(13.5, 27.8), (34, 29.2)], 2.0)                             # the rifle
    return s


def gogogo(src):
    p = Pic()
    frame_small(p)
    s = soldier_running(p.shape())
    sc = s.cov()
    p.lay(sc, GREEN, HALF)
    p.lay(outline(sc, 0.9), GREEN, LINE)
    ground = p.shape().line([(16, 53), (48, 53)], 1.4)
    p.lay(ground, GREEN, FAINT + 0.2)
    return p


def mountmg(src):
    p = Pic()
    frame_big(p)
    dk = (20, 26, 20)
    # the gunner behind it, low on the left: back, shoulders, helmet
    g = p.shape()
    g.poly([(7.5, 60), (7.5, 30), (13, 23), (22, 22), (29, 26), (31, 34), (27, 44), (24, 60)])
    g.ellipse(18, 18, 6.2, 5.5)
    g.rect(10.5, 17.5, 26, 19.5, r=1)
    gc = g.cov()
    p.lay(gc, GREEN, FAINT + 0.12)
    p.lay(outline(gc, 1.0), GREEN, LINE)
    arm = p.shape().line([(24, 31), (31, 28), (34, 24)], 3.4)
    p.lay(arm.cov(), GREEN, HALF)
    p.lay(outline(arm.cov(), 0.9), GREEN, LINE)
    # the machine gun: receiver, barrel with its jacket, the tripod's three legs
    mg = p.shape()
    mg.rect(28, 19, 42, 25.5, r=1.2)
    mg.rect(41, 20, 57, 23.4, r=0.6)
    mg.rect(54, 19.2, 57, 24.2, r=0.4)
    mg.line([(34, 25), (32, 29)], 2.2)                     # the grip
    mg.line([(39, 25), (26, 59)], 2.4).line([(39, 25), (47, 59)], 2.4).line([(40, 25), (55, 40)], 2.2)
    p.lay(mg, GREEN, LINE)
    holes = p.shape()
    for x in (44, 47, 50, 53):
        holes.rect(x, 21, x + 1.5, 22.4, r=0.3)
    p.lay(holes, dk, 0.8)
    return p


def openbox(src):
    p = Pic()
    frame_small(p)
    dk = (20, 26, 20)
    front = p.shape().poly([(14, 33), (34, 39), (34, 54), (14, 48)])
    side = p.shape().poly([(34, 39), (50, 32), (50, 47), (34, 54)])
    top_in = p.shape().poly([(14, 33), (30, 27), (50, 32), (34, 39)])
    lid = p.shape().poly([(30, 27), (50, 32), (54, 16), (34, 11)])
    p.lay(top_in, dk, 0.7)
    p.lay(lid, GREEN, FAINT)
    p.lay(outline(lid.cov(), 1.0), GREEN, LINE)
    p.lay(front, GREEN, LINE)
    p.lay(side, GREEN, HALF)
    p.lay(outline(side.cov(), 1.0), GREEN, LINE)
    a = p.shape()
    a.line([(26, 34), (37, 22)], 4.2, caps=False)
    a.poly([(33, 15), (44, 15), (42, 26)])
    p.lay(a, GREEN, LINE)
    band = p.shape().poly([(22, 35.4), (26, 36.6), (26, 51.6), (22, 50.4)])
    p.lay(band, dk, 0.45)
    return p


# --- the squad marks, the damage markers, the objectives, the reticle ----------------------------------------------

def arrowhead(sh, tip, back, length, width):
    """a filled arrow head at tip pointing away from back"""
    dx, dy = tip[0] - back[0], tip[1] - back[1]
    n = math.hypot(dx, dy)
    ux, uy = dx / n, dy / n
    bx, by = tip[0] - ux * length, tip[1] - uy * length
    sh.poly([tip, (bx - uy * width / 2, by + ux * width / 2), (bx + uy * width / 2, by - ux * width / 2)])


def gogo(cx, cy, r, tips):
    p = Pic()
    s = p.shape().ring(cx, cy, r, r, 3.0)
    for tx, ty in tips:
        ux, uy = tx - cx, ty - cy
        n = math.hypot(ux, uy)
        s.line([(cx, cy), (tx - ux / n * 7, ty - uy / n * 7)], 3.0)
        arrowhead(s, (tx, ty), (cx, cy), 10, 9.5)
    p.lay(s, GREEN, 1.0)
    return p


def gogo1(src):
    return gogo(32.25, 31.25, 18.25, [(7, 6), (57, 6), (6, 57), (58.5, 57)])


def gogo2(src):
    return gogo(32.75, 31.75, 18.25, [(2.5, 1.5), (62.5, 1.5), (1.5, 61.5), (62.5, 62.5)])


def overbuddy(src):
    p = Pic()
    s = p.shape().ring(31.5, 31.5, 18.0, 18.0, 2.9)
    c = 18.0 / math.sqrt(2)
    s.line([(31.5 - c, 31.5 + c), (31.5 + c, 31.5 - c)], 2.9)
    p.lay(s, GREEN, 1.0)
    return p


RED = (99, 40, 42)


def enemy_hit(src):
    """four wedges about the middle pointing in, strongest at their points, fading outwards"""
    p = Pic()
    xx, yy = p.grid()
    for fx, fy in ((1, 1), (-1, 1), (1, -1), (-1, -1)):
        X = (lambda x: x) if fx > 0 else (lambda x: 64 - x)
        Y = (lambda y: y) if fy > 0 else (lambda y: 64 - y)
        tri = p.shape().poly([(X(12.6), Y(4.5)), (X(4.0), Y(14.6)), (X(19.6), Y(19.9))])
        ox, oy, tx, ty = X(8.3), Y(9.5), X(19.6), Y(19.9)
        L = math.hypot(tx - ox, ty - oy)
        t = np.clip(((xx - ox) * (tx - ox) + (yy - oy) * (ty - oy)) / (L * L), 0, 1)
        p.lay(tri.cov() * (0.12 + 0.88 * t ** 1.4), RED, 0.74)
    return p


def hit5(src):
    """an arc under the middle, strongest at its middle, and a small point down at its middle"""
    p = Pic(64, 16)
    xx, yy = p.grid()
    cx, cy, R = 33.0, -18.1, 30.5
    band = p.shape()
    for a0, a1 in ((44, 82), (98, 136)):
        pts = [(cx + R * math.cos(math.radians(a)), cy + R * math.sin(math.radians(a))) for a in np.linspace(a0, a1, 40)]
        band.line(pts, 2.8)
    ang = np.degrees(np.arctan2(yy - cy, xx - cx))
    fade = np.clip(1 - np.abs(ang - 90) / 46, 0, 1) ** 0.8
    p.lay(band.cov() * (0.15 + 0.85 * fade), RED, 0.8)
    tri = p.shape().poly([(29.6, 9.2), (36.6, 9.2), (33.1, 14.6)])
    p.lay(tri, RED, 0.8)
    return p


GOLD = np.array([212, 172, 92], np.float32)


def medallion(p, cx, cy, disc_rgb):
    """a gold rim round a domed disc of disc_rgb, lit from the top left; returns the grid"""
    xx, yy = p.grid()
    d = np.hypot(xx - cx, yy - cy)
    lit = np.clip(1.1 - (((xx - cx) + (yy - cy)) / 15.5 + 1) * 0.35, 0.55, 1.15)[..., None]
    rim = p.shape().ellipse(cx, cy, 15.6)
    rim_shade = np.clip(0.75 + 0.5 * (1 - ((xx - cx) + (yy - cy)) / 22), 0.6, 1.3)[..., None]
    p.lay(rim, np.clip(GOLD * rim_shade, 0, 255), 1.0)
    edge = p.shape().ring(cx, cy, 14.0, 14.0, 0.6)
    p.lay(edge, (60, 45, 25), 0.7)
    disc = p.shape().ellipse(cx, cy, 13.6)
    dome = np.clip(1.15 - (d / 13.6) ** 2 * 0.45, 0.6, 1.2)[..., None]
    p.lay(disc, np.clip(np.asarray(disc_rgb, np.float32) * lit * dome, 0, 255), 1.0)
    return xx, yy


def star_pts(cx, cy, ro, ri, rot=-90):
    pts = []
    for k in range(10):
        r = ro if k % 2 == 0 else ri
        a = math.radians(rot + k * 36)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def allied_obj(src):
    p = Pic(32, 32)
    cx, cy = 16, 16
    medallion(p, cx, cy, (92, 92, 84))
    pts = star_pts(cx, cy + 0.6, 12.8, 5.1)
    p.lay(p.shape().poly(pts), (215, 160, 64), 1.0)
    # each point bevelled: its left half light, its right half in shade
    for k in range(0, 10, 2):
        tip = pts[k]
        left, right = pts[(k - 1) % 10], pts[(k + 1) % 10]
        p.lay(p.shape().poly([(cx, cy + 0.6), left, tip]), (246, 206, 112), 0.85)
        p.lay(p.shape().poly([(cx, cy + 0.6), tip, right]), (160, 110, 40), 0.55)
    return p


def german_obj(src):
    p = Pic(32, 32)
    cx, cy = 16, 16
    medallion(p, cx, cy, (150, 42, 36))
    white = p.shape().ellipse(cx, cy, 9.0)
    p.lay(white, (236, 232, 222), 1.0)
    shade = p.shape().ring(cx, cy, 8.6, 8.6, 0.9)
    p.lay(shade, (150, 140, 120), 0.6)
    cross = p.shape()
    w0, w1, L = 1.6, 4.2, 7.0  # the arms flaring from the middle out (a cross pattée)
    for ang in (0, 90, 180, 270):
        ca, sa = math.cos(math.radians(ang)), math.sin(math.radians(ang))
        pts = [(cx - sa * w0, cy + ca * w0), (cx + ca * L - sa * w1, cy + sa * L + ca * w1),
               (cx + ca * L + sa * w1, cy + sa * L - ca * w1), (cx + sa * w0, cy - ca * w0)]
        cross.poly(pts)
    cross.rect(cx - 2.2, cy - 2.2, cx + 2.2, cy + 2.2)
    cc = cross.cov()
    p.lay(cc, (240, 238, 230), 1.0)
    p.lay(erode(cc, 0.7 * K), (18, 16, 16), 1.0)
    return p


def t34_reticle(src):
    """the T34's sight: lines of two pixels, at the original's places"""
    p = Pic(128, 128)
    s = p.shape()
    s.rect(63, 4.5, 65, 20.5)
    for x0, x1 in ((18, 28), (35, 52), (76, 93), (100, 110)):
        s.rect(x0, 30.2, x1, 32.0)
    s.rect(63, 43, 65, 123)
    s.rect(43, 62.8, 85, 64.2)
    s.rect(52, 92.8, 76, 94.2)
    s.rect(58, 122, 70, 124)
    p.lay(s, (255, 255, 255), 1.0)
    return p


# --- the painted items: shaded solids ------------------------------------------------------------------------

def lit(p, cov, base, bump=2.0, spec=0.28, amb=0.42, shadow=0.5):
    """a solid of coverage cov in colour base, rounded at its edges (a dome bump wide), lit from the top left, a
    soft shadow under it"""
    if isinstance(cov, Shape):
        cov = cov.cov()
    if shadow:
        b = blur(cov, K * 1.1)
        dx, dy = int(1.2 * K), int(1.6 * K)
        sh = np.zeros_like(b)
        sh[dy:, dx:] = b[:-dy, :-dx]
        p.lay(sh, (0, 0, 0), shadow)
    h = blur(cov, K * bump)
    gy, gx = np.gradient(h)
    k = K * bump * 1.6
    nx, ny, nz = -gx * k, -gy * k, np.ones_like(h)
    n = np.sqrt(nx * nx + ny * ny + nz * nz)
    L = np.array([-0.55, -0.65, 0.75])
    L /= np.linalg.norm(L)
    dif = np.clip((nx * L[0] + ny * L[1] + nz * L[2]) / n, 0, 1)
    hv = L + np.array([0, 0, 1.0])
    hv /= np.linalg.norm(hv)
    sp = np.clip((nx * hv[0] + ny * hv[1] + nz * hv[2]) / n, 0, 1) ** 24 * spec
    rgb = np.asarray(base, np.float32)[None, None, :] * (amb + (1 - amb) * dif)[..., None] + 255 * sp[..., None]
    p.lay(cov, np.clip(rgb, 0, 255), 1.0)


def axis_pts(cx, cy, ang, pts):
    """points along an axis (u) and across it (v) placed at cx, cy turned by ang degrees"""
    c, s_ = math.cos(math.radians(ang)), math.sin(math.radians(ang))
    return [(cx + u * c - v * s_, cy + u * s_ + v * c) for u, v in pts]


def bar(sh, cx, cy, ang, u0, u1, wid, round_ends=False):
    sh.poly(axis_pts(cx, cy, ang, [(u0, -wid / 2), (u1, -wid / 2), (u1, wid / 2), (u0, wid / 2)]))
    if round_ends:
        for u in (u0, u1):
            (x, y), = axis_pts(cx, cy, ang, [(u, 0)])
            sh.ellipse(x, y, wid / 2)
    return sh


def oval(sh, cx, cy, ang, ru, rv, on=True):
    pts = [(ru * math.cos(t), rv * math.sin(t)) for t in np.linspace(0, 2 * math.pi, 72, endpoint=False)]
    return sh.poly(axis_pts(cx, cy, ang, pts), on)


def grenade_a(src):
    """a Mk 2 'pineapple': its grooved body, the fuze and lever at its top, the pin's ring"""
    p = Pic()
    cx, cy, ang = 33.5, 32.5, -51
    bc = oval(p.shape(), cx, cy, ang, 15.6, 11.4).cov()
    lit(p, bc, (88, 98, 70), bump=2.6)
    g = p.shape()
    for u in (-10.5, -5.2, 0, 5.2, 10.5):
        g.line(axis_pts(cx, cy, ang, [(u, -12), (u, 12)]), 1.0)
    for v in (-5.8, 0, 5.8):
        g.line(axis_pts(cx, cy, ang, [(-16, v), (16, v)]), 1.0)
    p.lay(g.cov() * erode(bc, 0.5 * K), (25, 30, 20), 0.6)
    lit(p, bar(p.shape(), cx, cy, ang, 14.5, 19.5, 7.0, True), (66, 72, 70), bump=1.0, shadow=0.3)
    lever = p.shape().line(axis_pts(cx, cy, ang, [(19.5, -2.8), (16, -10), (7, -12), (-3, -11.8)]), 2.1)
    lit(p, lever, (80, 88, 84), bump=0.6, shadow=0.35)
    (px, py), = axis_pts(cx, cy, ang, [(18.2, 5.6)])
    p.lay(p.shape().ring(px, py, 2.2, 2.2, 0.8), (150, 150, 140), 0.95)
    return p


def stick(src, handle, head, head_wid, rings=0, ribbed=False):
    """a stick grenade: the wooden (or metal) handle, its end cap, the head, its band"""
    p = Pic()
    cx, cy, ang = 31.0, 30.5, -38
    lit(p, bar(p.shape(), cx, cy, ang, -27.5, 6.5, 5.6, True), handle, bump=1.6)
    lit(p, bar(p.shape(), cx, cy, ang, -28.2, -25.5, 5.9, True), shade(handle, 0.7), bump=1.0, shadow=0)
    if rings:
        r = p.shape()
        for u in np.linspace(-20, -2, rings):
            r.line(axis_pts(cx, cy, ang, [(u, -2.9), (u, 2.9)]), 0.7)
        p.lay(r, (20, 16, 12), 0.45)
    lit(p, bar(p.shape(), cx, cy, ang, 5.5, 21.0, head_wid), head, bump=2.0)
    band = p.shape().line(axis_pts(cx, cy, ang, [(7.2, -head_wid / 2), (7.2, head_wid / 2)]), 1.1, caps=False)
    if ribbed:
        for u in (11, 14.5, 18):
            band.line(axis_pts(cx, cy, ang, [(u, -head_wid / 2), (u, head_wid / 2)]), 0.6, caps=False)
    p.lay(band, (25, 22, 18), 0.55)
    lit(p, bar(p.shape(), cx, cy, ang, 20.6, 22.0, head_wid * 0.62), shade(head, 0.8), bump=0.6, shadow=0)
    return p


def grenade_g(src):
    return stick(src, (128, 88, 74), (104, 98, 90), 9.6)


def grenade_r(src):
    return stick(src, (118, 100, 64), (138, 116, 72), 11.0, rings=6, ribbed=True)


def tellermine(src):
    """a Teller mine from above: the domed disc, its ring and spokes pressed in, the hub, the carrying handle"""
    p = Pic()
    cx, cy = 30.0, 32.5
    lit(p, p.shape().line([(41, 15), (46.5, 7.5), (53, 11.5), (49, 18.5)], 2.0), (150, 120, 70), bump=0.6)
    lit(p, p.shape().ellipse(cx, cy, 19.6, 19.0), (186, 152, 92), bump=3.0, spec=0.2)
    g = p.shape().ring(cx, cy, 14.2, 13.8, 1.0).ring(cx, cy, 4.6, 4.4, 1.2)
    hi = p.shape()
    for k in range(5):
        a = math.radians(-72 + k * 72)
        g.line([(cx + 5.6 * math.cos(a), cy + 5.4 * math.sin(a)), (cx + 13.4 * math.cos(a), cy + 13 * math.sin(a))], 1.8)
        a -= 0.08
        hi.line([(cx + 5.8 * math.cos(a), cy + 5.6 * math.sin(a)), (cx + 13.2 * math.cos(a), cy + 12.8 * math.sin(a))], 0.6)
    p.lay(g, (90, 66, 30), 0.6)
    p.lay(hi, (240, 214, 150), 0.55)
    lit(p, p.shape().ellipse(cx, cy, 3.3), (170, 138, 80), bump=0.8, shadow=0)
    return p


def stickymine(src):
    """a sticky bomb: the black ball, its collar, the long handle"""
    p = Pic()
    lit(p, p.shape().line([(31, 37), (7.5, 56)], 4.0), (205, 205, 200), bump=1.2, spec=0.4)
    lit(p, p.shape().line([(36, 33), (29, 39)], 6.0), (60, 60, 62), bump=1.2, spec=0.4)
    lit(p, p.shape().ellipse(42, 26.5, 14.2), (64, 64, 68), bump=4.5, spec=0.75, amb=0.3)
    return p


def inv_radio(src):
    """a field radio: its case and handle, the panel with its dial, grille and knobs"""
    p = Pic()
    handle = p.shape().rect(24, 10.5, 37, 16.5, r=2.8)
    handle.rect(26.4, 12.6, 34.6, 16.5, r=1.2, on=False)
    lit(p, handle, (190, 180, 160), bump=0.6)
    lit(p, p.shape().rect(11, 15.5, 50.5, 48, r=1.8), (156, 142, 112), bump=1.4, spec=0.12)
    p.lay(p.shape().rect(14, 19, 47.5, 44.5, r=1.0), (60, 52, 40), 0.45)
    lit(p, p.shape().ellipse(24, 30, 6.0), (210, 200, 175), bump=1.2, shadow=0.25)
    p.lay(p.shape().line([(24, 30), (27.5, 26)], 0.7), (40, 30, 25), 0.9)
    gr = p.shape()
    for x in np.arange(34.5, 46, 2.0):
        gr.line([(x, 22), (x, 33)], 0.8)
    p.lay(gr, (40, 34, 26), 0.6)
    for x in (18, 26, 34, 42):
        lit(p, p.shape().ellipse(x, 40, 1.9), (40, 38, 34), bump=0.6, spec=0.5, shadow=0.2)
    return p


def bear(src, fur, helmet=None, top=3.5):
    """a teddy bear sitting up, arms out; a spiked helmet (a pickelhaube) on the Kaiser's"""
    p = Pic()
    y = top + (5.0 if helmet else 0)  # (under a helmet the head sits lower)
    body = p.shape()
    body.ellipse(32, y + 35.5, 11.5, 14.5)
    body.line([(24.5, y + 26), (16, y + 32.5)], 7.6).line([(39.5, y + 26), (48, y + 32.5)], 7.6)
    body.line([(26, y + 45), (22, y + 53)], 8.6).line([(38, y + 45), (42, y + 53)], 8.6)
    lit(p, body, fur, bump=3.0, spec=0.06)
    p.lay(blur(p.shape().ellipse(32, y + 37.5, 6.6, 8.6).cov(), K * 0.8), shade(fur, 1.25), 0.7)
    p.lay(p.shape().ellipse(21.6, y + 54.4, 3.2, 2.6).ellipse(42.4, y + 54.4, 3.2, 2.6), shade(fur, 1.25), 0.75)
    head = p.shape().ellipse(32, y + 14.5, 10.4, 9.6)
    if not helmet:
        head.ellipse(22.8, y + 6.2, 4.2).ellipse(41.2, y + 6.2, 4.2)
    lit(p, head, fur, bump=3.0, spec=0.06)
    if not helmet:
        p.lay(p.shape().ellipse(22.8, y + 6.2, 2.0).ellipse(41.2, y + 6.2, 2.0), shade(fur, 0.7), 0.7)
    lit(p, p.shape().ellipse(32, y + 18.5, 5.2, 3.8), shade(fur, 1.25), bump=1.2, shadow=0, spec=0.05)
    dark = p.shape().ellipse(32, y + 16.6, 2.0, 1.4).ellipse(27.6, y + 12.6, 1.3).ellipse(36.4, y + 12.6, 1.3)
    dark.line([(32, y + 17.5), (32, y + 20.0)], 0.6)
    p.lay(dark, (24, 20, 18), 0.92)
    if helmet:
        hm = p.shape().ellipse(32, y + 8.4, 10.2, 7.2)
        hm.rect(18.5, y + 8.0, 45.5, y + 11.0, r=1.2)
        hm.rect(30.4, y - 6.0, 33.6, y + 1.5, r=1.0)
        hm.ellipse(32, y - 6.4, 1.9)
        lit(p, hm, helmet, bump=1.6, spec=0.5)
        p.lay(p.shape().poly([(32, y + 2.5), (35.2, y + 5.5), (32, y + 8.5), (28.8, y + 5.5)]), shade(helmet, 1.5), 0.85)
    return p


def bear_health(src):
    return bear(src, (132, 124, 100))


def kaiser_bear(src):
    return bear(src, (118, 112, 96), helmet=(96, 96, 100), top=4)


def sticky_bear(src):
    return bear(src, (226, 80, 122), helmet=(222, 92, 132), top=2)


def dynamite(p, cx, cy, ang, length, scale=1.0, fuse=True):
    """three sticks of dynamite taped together"""
    sticks = p.shape()
    for v in (-4.4, 0, 4.4):
        (x, y), = axis_pts(cx, cy, ang, [(0, v * scale)])
        bar(sticks, x, y, ang, -length / 2, length / 2, 4.3 * scale, True)
    sc = sticks.cov()
    lit(p, sc, (184, 146, 104), bump=1.2 * scale, spec=0.1)
    ends = p.shape()
    for v in (-4.4, 0, 4.4):
        (x, y), = axis_pts(cx, cy, ang, [(-length / 2, v * scale)])
        ends.ellipse(x, y, 1.4 * scale)
    p.lay(ends, (90, 64, 40), 0.7)
    tape = p.shape()
    for u in (-length / 4, length / 4):
        tape.line(axis_pts(cx, cy, ang, [(u, -7 * scale), (u, 7 * scale)]), 2.2 * scale, caps=False)
    p.lay(tape.cov() * sc, (24, 20, 18), 0.92)
    if fuse:
        (x0, y0), = axis_pts(cx, cy, ang, [(length / 2 + 0.6, 0)])
        p.lay(p.shape().line([(x0, y0), (x0 + 2.4, y0 - 2.2), (x0 + 4.2, y0 - 2.0)], 0.9), (40, 34, 28), 0.9)


def bomb_location(src):
    p = Pic(32, 32)
    dynamite(p, 14.2, 17.8, -43, 24, 0.95)
    return p


def bomb_location_dest(src):
    p = Pic(32, 32)
    lit(p, p.shape().ellipse(16, 16.5, 13.6), (196, 196, 200), bump=1.5, spec=0.1, shadow=0.35)
    dynamite(p, 16, 16.5, -43, 21, 0.8, fuse=False)
    ring = p.shape().ring(16, 16.5, 13.6, 13.6, 3.6)
    c = 13.6 / math.sqrt(2)
    ring.line([(16 - c, 16.5 - c), (16 + c, 16.5 + c)], 3.6)
    lit(p, ring, (206, 36, 62), bump=0.9, spec=0.35, shadow=0.3)
    return p


def bomb_placed(src):
    p = Pic(32, 32)
    dynamite(p, 11.5, 20.5, -43, 22, 0.9, fuse=False)
    lit(p, p.shape().ellipse(21.5, 10.5, 9.4), (232, 232, 226), bump=1.6, spec=0.3)
    p.lay(p.shape().ring(21.5, 10.5, 9.0, 9.0, 0.9), (130, 130, 128), 0.85)
    t = p.shape()
    for k in range(12):
        a = math.radians(k * 30)
        r0 = 6.0 if k % 3 else 5.2
        t.line([(21.5 + r0 * math.cos(a), 10.5 + r0 * math.sin(a)), (21.5 + 7.2 * math.cos(a), 10.5 + 7.2 * math.sin(a))], 0.55)
    t.line([(21.5, 10.5), (21.5, 4.8)], 0.8).line([(21.5, 10.5), (24.6, 11.6)], 0.8)
    p.lay(t, (30, 28, 26), 0.92)
    p.lay(p.shape().line([(14.5, 15.5), (16.5, 13.5)], 0.9), (150, 30, 30), 0.9)
    return p


# --- the flags, the frames, the buddies ---------------------------------------------------------------------------

def pattern(kind, u, v):
    """a flag's colours at (u, v), 0..1 across and down"""
    rgb = np.zeros(u.shape + (3,), np.float32)
    if kind == "american":
        stripe = np.floor(v * 13).astype(int) % 2 == 0
        rgb[:] = np.where(stripe[..., None], [178, 34, 52], [240, 240, 236])
        canton = (u < 0.4) & (v < 7 / 13)
        rgb[canton] = [40, 52, 120]
        su, sv = (u / 0.4) * 8, (v / (7 / 13)) * 6
        star = canton & (np.hypot(su - np.floor(su) - 0.5, sv - np.floor(sv) - 0.5) < 0.22)
        rgb[star] = [240, 240, 236]
    elif kind == "british":
        rgb[:] = [30, 40, 110]
        x, y = u * 2 - 1, v * 2 - 1
        d1, d2 = np.abs(x * 0.5 - y * 1.0), np.abs(x * 0.5 + y * 1.0)
        diag = np.minimum(d1, d2) / math.hypot(0.5, 1.0)
        rgb[diag < 0.13] = [240, 240, 236]
        red_diag = ((d1 / math.hypot(0.5, 1.0) < 0.05) & (x * y > 0)) | ((d2 / math.hypot(0.5, 1.0) < 0.05) & (x * y < 0))
        rgb[red_diag] = [200, 16, 46]
        white_cross = (np.abs(x) < 0.17) | (np.abs(y) < 0.3)
        rgb[white_cross] = [240, 240, 236]
        red_cross = (np.abs(x) < 0.1) | (np.abs(y) < 0.18)
        rgb[red_cross] = [200, 16, 46]
    elif kind == "german":
        rgb[:] = np.where((v < 1 / 3)[..., None], [24, 22, 22], np.where((v < 2 / 3)[..., None], [236, 236, 232], [205, 40, 40]))
    elif kind == "soviet":
        rgb[:] = [204, 20, 40]
        X, Y = (u - 0.17) * 1.6, v - 0.34  # (square units about the emblem)
        r = np.hypot(X + 0.01, Y)
        ang = np.degrees(np.arctan2(Y, X + 0.01))
        sickle = (np.abs(r - 0.085) < 0.022) & ((ang > 120) | (ang < 20))
        handle = (np.abs(X + Y - 0.0) < 0.022) & (X < -0.03) & (X > -0.1) & (Y > 0.03)
        hx, hy = X * 0.707 - Y * 0.707, X * 0.707 + Y * 0.707  # the hammer, turned
        hammer = ((np.abs(hx) < 0.018) & (hy > -0.09) & (hy < 0.09)) | ((np.abs(hy + 0.08) < 0.022) & (np.abs(hx) < 0.05))
        sx, sy = X - 0.0, Y + 0.17
        sr = np.hypot(sx, sy)
        sa = np.arctan2(sy, sx) + math.pi / 2
        star = sr < 0.045 * (0.55 + 0.45 * np.abs(np.cos(sa * 2.5)))
        rgb[sickle | handle | hammer | star] = [236, 190, 60]
    return rgb


def flag_rect(kind):
    p = Pic(64, 32)
    xx, yy = p.grid()
    s = p.shape().rect(0, 0, 64, 32, r=0.8)
    rgb = pattern(kind, np.clip((xx - 1) / 62, 0, 1), np.clip((yy - 1) / 30, 0, 1))
    cloth = 0.92 + 0.08 * np.sin(xx / 64 * 6.3 + yy / 32 * 1.5)[..., None]
    p.lay(s, np.clip(rgb * cloth, 0, 255), 1.0)
    edge = p.shape().rect(0, 0, 64, 32, r=0.8)
    edge.rect(1.2, 1.2, 62.8, 30.8, r=0.4, on=False)
    p.lay(edge, (190, 190, 186), 1.0)
    p.out[..., 3] *= 0.68
    return p


def ctf_border(src):
    p = Pic(64, 32)
    s = p.shape().rect(0, 0, 64, 32, r=1.2)
    s.rect(2, 2, 62, 30, r=0.6, on=False)
    p.lay(s, (255, 255, 255), 1.0)
    return p


def flag_wave(kind):
    """a flag on its pole, flying: its cloth waved, its colours following the wave, the folds shaded"""
    p = Pic()
    xx, yy = p.grid()
    pole = p.shape().line([(21.6, 13), (14.6, 53)], 1.5)
    lit(p, pole, (90, 86, 80), bump=0.4, shadow=0.4)
    p.lay(p.shape().ellipse(21.7, 12.6, 1.3), (200, 190, 150), 1.0)
    x0, x1 = 21.6, 48.5
    u = (xx - x0) / (x1 - x0)
    phase = u * 2 * math.pi * 1.15
    wave = 2.4 * np.sin(phase) * (0.3 + 0.7 * u)
    top = 13.6 + wave + u * 1.2
    bot = 31.0 + wave - u * 0.6
    v = (yy - top) / (bot - top)
    inside = (u >= 0) & (u <= 1) & (v >= 0) & (v <= 1)
    cov = inside.astype(np.float32)
    cov = blur(cov, 0.6)  # (its edge antialiased)
    rgb = pattern(kind, np.clip(u, 0, 1), np.clip(v, 0, 1))
    fold = (0.78 + 0.32 * np.cos(phase - 0.6))[..., None]
    sh = np.zeros_like(cov)
    dx, dy = int(1.2 * K), int(1.6 * K)
    sh[dy:, dx:] = cov[:-dy, :-dx]
    p.lay(blur(sh, K), (0, 0, 0), 0.4)
    p.lay(cov, np.clip(rgb * fold, 0, 255), 1.0)
    return p


def fist(src):
    """a raised fist, red: the back of the hand, four curled fingers, the thumb across"""
    p = Pic()
    red = (160, 30, 30)
    lit(p, p.shape().rect(18, 30, 47, 54, r=6).rect(22, 48, 43, 62, r=3), red, bump=3.0, spec=0.15)
    for i, (x, y) in enumerate(((19, 20.5), (26, 18.5), (33, 18.5), (40, 20.5))):
        lit(p, p.shape().rect(x - 0.5, y, x + 7.5, y + 13, r=3.6), shade(red, 1.05 - 0.04 * i), bump=2.0, spec=0.2, shadow=0.25)
    lit(p, p.shape().line([(20.5, 37.5), (27, 34), (34.5, 34.5)], 5.6), shade(red, 1.1), bump=1.8, spec=0.2, shadow=0.35)
    k = p.shape()
    for x in (25.5, 32.5, 39.5):
        k.line([(x, 22), (x, 29)], 0.7)
    p.lay(k, (60, 8, 8), 0.6)
    return p


GREY = (178, 178, 178)


def act_frame(icon):
    """the bomb's and the defuse's progress frame: a grey line round the bar, the action's icon on the left"""
    p = Pic(128, 64)
    s = p.shape().rect(1, 20, 128, 44, r=1.0)
    s.rect(3, 22, 126, 42, r=0.4, on=False)
    s.rect(1, 20, 21, 44, r=1.0)
    p.lay(s, GREY, 0.78)
    ic = p.shape()
    if icon == "bomb":
        ic.ellipse(9.5, 33.5, 6.4)
        ic.poly([(13, 28), (16, 25.5), (18, 28), (15, 30.5)])
        ic.line([(17, 26.5), (18, 23.5)], 0.9)
        p.lay(ic, (30, 30, 30), 0.95)
        p.lay(blur(p.shape().ellipse(7.5, 31.5, 1.8).cov(), K * 0.6), (200, 200, 200), 0.7)
    else:
        ic.line([(13, 30), (9, 35), (4.5, 39.5)], 2.6).line([(13, 30), (12.5, 36), (10, 40.5)], 2.6)
        ic.poly([(11.5, 28.5), (15, 23), (17, 22.5), (14.5, 30.5)]).poly([(13.5, 31.5), (18.5, 26.5), (19.5, 28), (15, 32)])
        p.lay(ic, (30, 30, 30), 0.95)
    return p


def bomb_frame(src):
    return act_frame("bomb")


def defuse_frame(src):
    return act_frame("defuse")


LIGHT = (205, 205, 205)


def side_tank(p, kind):
    """a tank from the side, facing left: tracks and road wheels, hull, the turret or the open gun deck, the gun"""
    body = p.shape()
    if kind == "t34":
        body.poly([(17, 15.5), (24, 11), (44, 11), (48.5, 16), (48.5, 21), (16, 21)])   # sloped hull
        body.rect(23.5, 7, 38, 12, r=1.8)                                                  # turret
        body.rect(13, 8.2, 24.5, 9.8, r=0.5)                                               # gun
    else:
        body.poly([(16, 16), (20, 12), (46, 12), (47.5, 16.5), (47.5, 21), (15.5, 21)])  # hull
        body.poly([(29, 8.6), (44, 8.6), (46, 12), (27, 12)])                             # the open gun deck
        body.rect(15, 8.4, 32, 10.4, r=0.6)                                                # the big gun
        body.ellipse(43.6, 15, 3.2, 4.2)                                                   # the spade at the back
    body.rect(15.5, 18, 48.5, 24.6, r=3.3)                                                 # tracks
    bc = body.cov()
    p.lay(blur(bc, K * 1.0), (20, 20, 20), 0.35)
    p.lay(bc, (120, 120, 120), 0.45)
    p.lay(outline(bc, 0.8), LIGHT, 0.75)
    w = p.shape()
    for x in np.linspace(19.5, 44.5, 6):
        w.ring(x, 21.4, 1.9, 1.9, 0.7)
    p.lay(w, LIGHT, 0.7)


def m12(src):
    p = Pic(64, 32)
    side_tank(p, "m12")
    return p


def t34_side(src):
    p = Pic(64, 32)
    side_tank(p, "t34")
    return p


def person(src):
    """a soldier walking with his rifle, grey, a light outline"""
    p = Pic(32, 32)
    s = p.shape()
    s.ellipse(16.5, 7.2, 2.3, 2.3)
    s.ellipse(16.6, 6.2, 3.2, 2.2)
    s.rect(13.0, 6.4, 20.2, 7.4, r=0.4)
    s.poly([(13.5, 9.6), (19.5, 9.4), (20.5, 15), (19, 19.2), (14, 19.2), (12.8, 14)])
    s.line([(15, 18.5), (12.5, 23), (9.6, 27.2)], 3.8).line([(18, 18.5), (20.5, 23), (23.4, 27.4)], 3.8)
    s.line([(14, 11), (11.5, 14), (8.5, 13.6)], 2.6).line([(19, 11), (17, 15), (13, 14.6)], 2.6)
    s.line([(5.5, 13.4), (17.5, 14.8)], 1.4)
    s.poly([(12.6, 10.2), (20.6, 9.8), (21.4, 16), (19.6, 20), (13.6, 20), (12, 15)])
    sc = s.cov()
    p.lay(blur(sc, K * 0.8), (20, 20, 20), 0.35)
    p.lay(sc, (120, 120, 120), 0.45)
    p.lay(outline(sc, 0.7), LIGHT, 0.7)
    return p


def sherman_turret(src):
    """a Sherman's turret from above: round, its gun forward (up), the hatches"""
    p = Pic()
    t = p.shape().ellipse(32, 41, 9.6, 9.2).rect(29.6, 30, 34.4, 34.5, r=1).rect(31.1, 12.5, 32.9, 32, r=0.5)
    tc = t.cov()
    p.lay(blur(tc, K * 0.8), (20, 20, 20), 0.35)
    p.lay(tc, (150, 150, 150), 0.6)
    p.lay(outline(tc, 0.75), LIGHT, 0.8)
    h = p.shape().ring(35.5, 44, 3.0, 3.0, 0.8)
    h.rect(25.5, 39.5, 30.5, 45.5, r=0.6)
    h.rect(26.6, 40.6, 29.4, 44.4, r=0.3, on=False)
    p.lay(h, LIGHT, 0.75)
    return p


# --- the registry -------------------------------------------------------------------------------------------------

ICONS = {
    "hud_action_armmine": armmine, "hud_action_disarmmine": disarmmine, "hud_action_dismount": dismount,
    "hud_action_drivevehicle": drivevehicle, "hud_action_get": get, "hud_action_healfriendly": healfriendly,
    "hud_action_opendoor": opendoor, "hud_action_pickupweapon": pickupweapon, "hud_action_plantbomb": plantbomb,
    "hud_action_radio": radio, "hud_action_bomb": bomb, "hud_action_defuse": defuse, "hud_action_gogogo": gogogo,
    "hud_action_mountMG": mountmg, "hud_action_openbox": openbox,
    "hud_crosshair_gogo1": gogo1, "hud_crosshair_gogo2": gogo2, "hud_crosshair_overbuddy": overbuddy,
    "hud_enemy_hit_indicator": enemy_hit, "hud_hit_indicator5": hit5, "hud_allied_objective": allied_obj,
    "hud_german_objective": german_obj, "hud_T34_reticle": t34_reticle,
    "hud_inventory_grenadeA": grenade_a, "hud_inventory_grenadeG": grenade_g, "hud_inventory_grenadeR": grenade_r,
    "hud_inventory_tellermine": tellermine, "hud_inventory_stickymine": stickymine, "hud_inventory_radio": inv_radio,
    "hud_inventory_BearHealth": bear_health, "hud_inventory_KaiserBear": kaiser_bear,
    "hud_inventory_StickyBear": sticky_bear, "hud_bomb_location": bomb_location,
    "hud_bomb_location_dest": bomb_location_dest, "hud_bomb_placed": bomb_placed,
    "hud_ctf_american": lambda s: flag_rect("american"), "hud_ctf_british": lambda s: flag_rect("british"),
    "hud_ctf_german": lambda s: flag_rect("german"), "hud_ctf_soviet": lambda s: flag_rect("soviet"),
    "hud_ctf_border": ctf_border, "hud_ctf_flag_captured": fist,
    "hud_ctf_flag_icon_american": lambda s: flag_wave("american"), "hud_ctf_flag_icon_british": lambda s: flag_wave("british"),
    "hud_ctf_flag_icon_german": lambda s: flag_wave("german"), "hud_ctf_flag_icon_soviet": lambda s: flag_wave("soviet"),
    "hud_bomb_frame": bomb_frame, "hud_defuse_frame": defuse_frame, "hud_M12_buddy_health": m12,
    "hud_T34_buddy_health": t34_side, "hud_person_buddy_health": person, "hud_sherman_turret": sherman_turret,
}


# the painted items, fitted after drawing into the original's own box (its middle and size, from its alpha): the game
# places them by that, a drawing a little off its box sat off-centre in the inventory's frame
FIT = {"hud_inventory_grenadeA", "hud_inventory_grenadeG", "hud_inventory_grenadeR", "hud_inventory_tellermine",
       "hud_inventory_stickymine", "hud_inventory_radio", "hud_inventory_BearHealth", "hud_inventory_KaiserBear",
       "hud_inventory_StickyBear", "hud_bomb_location", "hud_bomb_location_dest", "hud_bomb_placed",
       "hud_ctf_flag_captured"}


# the inventory slot: the game draws its item's quad (console 40..102 x 53..117) left of the middle of its frame's box
# (the frame 28..111 x 51..119, its box's inside 15..62 x 2..61 of its 64): that middle in the item's own texels, and
# how far the drawing may reach in them (the item's quad ends before the box does)
SLOT_MIDDLE = (39.2, 31.5)
SLOT_REACH = (3.0, 3.0, 62.0, 61.0)
# the counted items (the grenades, the mines, the bears) all in one place and at one size, clear of the count the
# game writes over the slot's lower right (37..50 x 41..55 of the item's texels): their middle and their largest side
# (in the middle, the Teller mine's disc covered the count and the grenades touched it)
SLOT_PLACE = {n: ((31.5, 25.5), 32.0) for n in (
    "hud_inventory_grenadeA", "hud_inventory_grenadeG", "hud_inventory_grenadeR", "hud_inventory_tellermine",
    "hud_inventory_stickymine", "hud_inventory_KaiserBear", "hud_inventory_StickyBear")}


def fit(out, src, slot=False, name=""):
    """out (K times, straight alpha) moved and scaled so its drawing fills the original's box: a scale per axis, the
    two kept within 12% of each other (the shapes' proportions)"""
    oa = src[..., 3] > 128
    da = out[..., 3] > 128
    if not oa.any() or not da.any():
        return out
    ys, xs = np.nonzero(oa)
    t = np.array([xs.min(), ys.min(), xs.max() + 1, ys.max() + 1], np.float64) * K
    if slot:
        # in the middle of the frame's box, as large as the original's, smaller if it would leave the quad
        w, h = (t[2] - t[0]) / K, (t[3] - t[1]) / K
        cx, cy = SLOT_MIDDLE
        k = min(1.0, 2 * (cx - SLOT_REACH[0]) / w, 2 * (SLOT_REACH[2] - cx) / w,
                2 * (cy - SLOT_REACH[1]) / h, 2 * (SLOT_REACH[3] - cy) / h)
        if name in SLOT_PLACE:
            (cx, cy), side = SLOT_PLACE[name]
            k = side / max(w, h)
        t = np.array([cx - w * k / 2, cy - h * k / 2, cx + w * k / 2, cy + h * k / 2]) * K
    ys, xs = np.nonzero(da)
    d = np.array([xs.min(), ys.min(), xs.max() + 1, ys.max() + 1], np.float64)
    sx, sy = (t[2] - t[0]) / (d[2] - d[0]), (t[3] - t[1]) / (d[3] - d[1])
    m = math.sqrt(sx * sy)
    sx, sy = np.clip(sx, m / 1.06, m * 1.06), np.clip(sy, m / 1.06, m * 1.06)
    tcx, tcy = (t[0] + t[2]) / 2, (t[1] + t[3]) / 2
    dcx, dcy = (d[0] + d[2]) / 2, (d[1] + d[3]) / 2
    H, W = out.shape[:2]
    pm = out.copy()
    pm[..., :3] *= pm[..., 3:4] / 255
    # (the inverse: an output pixel's place in the drawing)
    coeffs = (1 / sx, 0, dcx - tcx / sx, 0, 1 / sy, dcy - tcy / sy)
    ch = [np.asarray(Image.fromarray(pm[..., c].astype(np.float32), "F").transform((W, H), Image.AFFINE, coeffs,
                                                                                   Image.BICUBIC)) for c in range(4)]
    res = np.stack(ch, -1)
    a = np.clip(res[..., 3:4], 0, 255)
    res[..., :3] = np.where(a > 0.5, res[..., :3] * 255 / np.maximum(a, 1e-3), 0)
    res[..., 3:4] = a
    return np.clip(res, 0, 255)


def main():
    extract, pack = sys.argv[1], sys.argv[2]
    only = set(sys.argv[3].split(",")) if len(sys.argv) > 3 else None
    index = json.load(open(os.path.join(extract, "index.json")))
    work = os.path.join(pack, "work_hud3")
    os.makedirs(work, exist_ok=True)
    files = []
    for rid, info in index.items():
        name = info["names"][0]
        if name not in ICONS or (only and name not in only):
            continue
        src = np.asarray(Image.open(os.path.join(extract, rid + ".png")).convert("RGBA")).astype(np.float32)
        p = ICONS[name](src)
        out = p.out if isinstance(p, Pic) else p
        if name in FIT:
            out = fit(out, src, slot=name.startswith("hud_inventory_"), name=name)
        path = os.path.join(work, rid + ".c.png")
        Image.fromarray((np.clip(out, 0, 255) + 0.5).astype(np.uint8), "RGBA").save(path)
        files.append(path)
        print(rid, name, flush=True)
    generate.texconv(files, "BC7_UNORM", os.path.join(pack, "textures"))


if __name__ == "__main__":
    main()
