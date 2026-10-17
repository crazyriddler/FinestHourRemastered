"""The kinds of graphic defects found in the port before, looked for in a screenshot (world_tour.py): a thin line
running across a smooth area (the sky's seams), sparkles (lone bright pixels on a dark surface: glints that should
not be), black holes in a lit place (a blot, a hole in the depth), magenta (a texture missing). Returns short
descriptions, empty when nothing is suspect. The HUD's corners are left out.

    python tools/defects.py <image>...
"""
import sys

import numpy as np
from PIL import Image


def box(a, r, rx=None):
    """the mean of a over a (2r+1) by (2rx+1) box, edges clamped (integral image)"""
    rx = r if rx is None else rx
    p = np.pad(a, ((r + 1, r + 1), (rx + 1, rx + 1)), mode="edge").astype(np.float64)
    c = p.cumsum(0).cumsum(1)
    ky, kx = 2 * r + 1, 2 * rx + 1
    s = c[ky:, kx:] - c[:-ky, kx:] - c[ky:, :-kx] + c[:-ky, :-kx]
    return (s / (ky * kx))[: a.shape[0], : a.shape[1]].astype(np.float32)


def runs(mask):
    """the longest run of True along each row"""
    best = np.zeros(mask.shape[0], np.int32)
    cur = np.zeros(mask.shape[0], np.int32)
    for x in range(mask.shape[1]):
        cur = np.where(mask[:, x], cur + 1, 0)
        best = np.maximum(best, cur)
    return best


def lines(L, smooth, axis_name):
    """thin lines (1 or 2 pixels) across smooth ground: the second difference across them strong, the ground two
    pixels off on either side alike"""
    up, dn = np.roll(L, 2, 0), np.roll(L, -2, 0)
    up1, dn1 = np.roll(L, 1, 0), np.roll(L, -1, 0)
    thin = (np.abs(L - 0.5 * (up + dn)) > 10) & (np.abs(up - dn) < 6) & (np.abs(up1 - dn1) < 14)
    thin &= smooth
    thin[:3] = False
    thin[-3:] = False
    # (gaps of a few pixels bridged: noise breaks a faint line here and there)
    bridged = thin.copy()
    for d in range(1, 5):
        bridged |= np.roll(thin, d, 1) & np.roll(thin, -(5 - d), 1)
    r = runs(bridged)
    w = L.shape[1]
    rows = np.nonzero(r > 0.18 * w)[0]
    if len(rows) == 0:
        return []
    return ["%s line at %d (%d px long)" % (axis_name, int(rows[0]), int(r[rows].max()))]


def check(im):
    a = np.asarray(im.convert("RGB")).astype(np.float32)
    h, w = a.shape[:2]
    # the HUD: the compass and weapon corners, the bottom right
    hud = np.zeros((h, w), bool)
    hud[int(h * 0.62):, : int(w * 0.25)] = True
    hud[int(h * 0.75):, int(w * 0.72):] = True
    L = a @ np.array([0.299, 0.587, 0.114], np.float32)
    found = []
    # lines over smooth areas (the sky): the local spread low
    # (smooth on both sides of it: the line itself would make its own place look rough)
    mean = box(L, 3)
    var = box(L * L, 3) - mean * mean
    rough = var > 40
    sm_h = ~np.roll(rough, 5, 0) & ~np.roll(rough, -5, 0) & ~hud
    sm_v = ~np.roll(rough, 5, 1) & ~np.roll(rough, -5, 1) & ~hud
    found += lines(L, sm_h, "horizontal")
    found += lines(L.T.copy(), sm_v.T.copy(), "vertical")
    # sparkles: lone bright pixels on a darker surface (not text, not the crosshair: those come in crowds)
    hud[int(h * 0.40):int(h * 0.60), int(w * 0.44):int(w * 0.56)] = True
    m9 = box(L, 4)
    sp = (L > 200) & (m9 < 70) & ~hud
    # (text: its letters' strokes side by side along a line)
    crowd = box(sp.astype(np.float32), 10) * 441
    row = box((L > 170).astype(np.float32), 7, 60) * 15 * 121
    lone = sp & (crowd <= 6) & (row <= 10)
    nsp = int(lone.sum())
    if 3 <= nsp <= 3000:
        ys, xs = np.nonzero(lone)
        found.append("%d lone sparkles (first at %d,%d)" % (nsp, int(xs[0]), int(ys[0])))
    # black holes in a lit place
    # (most of the place about lit, not a bright icon or text over darkness)
    m31 = box(L, 15)
    lit = box((L > 40).astype(np.float32), 15)
    hole = (L < 2) & (m31 > 70) & (lit > 0.6) & ~hud
    nh = int(hole.sum())
    if nh > 1500:
        ys, xs = np.nonzero(hole)
        found.append("%d black pixels in a lit place (about %d,%d)" % (nh, int(np.median(xs)), int(np.median(ys))))
    # magenta: a texture missing
    mg = (a[..., 0] > 180) & (a[..., 2] > 180) & (a[..., 1] < 70) & ~hud
    if int(mg.sum()) > 100:
        found.append("%d magenta pixels" % int(mg.sum()))
    return found


if __name__ == "__main__":
    for p in sys.argv[1:]:
        print(p, check(Image.open(p)))
