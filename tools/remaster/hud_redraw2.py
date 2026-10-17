"""The rest of the in-game HUD drawn again as vector art (the compass and its stars excepted, the ammunition, health,
inventory, medkit and postures being hud_redraw.py's): the action icons, the crosshairs' squad marks, the damage
markers, the tanks' and buddies' outlines, the T34's reticle, the frames, the inventory's items, the objectives and
flags. At 8 times the original's size, each in its place in its texture, none of the original's pixels kept.

The one coloured ones (the action icons, the squad marks, the frames, the reticle, the damage markers) are split into
their alpha's tones (a dithered tone smoothed into the one it makes), each tone's regions traced as smooth curves
(marching squares, then simplified to straight runs that keep their corners and rounded where they bend) and stacked.
The painted ones (the grenades, the mines, the bears, the flags) keep the pack's AI colour for their detail, their
silhouette traced clean and their colour spread past it (the pack's AI colour of each kept the first time in
<pack>/work_hud2_src). The tanks' and buddies' outlines are drawn as the postures are (their silhouette traced whole, an unbroken line along it,
a see-through fill and a soft halo), the tank's aiming ring as a true circle. The T34's own hull and turret (the tank's view) are drawn from scratch as
the medkit is, at the originals' places.

    python -I hud_redraw2.py <extract folder> <pack folder> [names or ids,...]
"""
import json
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate  # noqa: E402
from hud_redraw import K, SS, Canvas, blur, contours  # noqa: E402

DONE = {"f62d1fded3b3cc9a", "7c691e4b5741194e", "55611bd9a3025440", "0b41acaa2b79af57", "78aa5a7bf1a08670",
        "a4db31f92c1ea1a4", "e67bb8cbd91d5674"}
OUTLINED = {"hud_sherman_turret", "hud_T34_buddy_health", "hud_M12_buddy_health",
            "hud_person_buddy_health"}
RING = {"hud_tank_arrow"}
# one hue in a few shades (the action icons drawn with shading, the frames): its tones by brightness and opacity
TONED = {"hud_action_bomb", "hud_action_defuse", "hud_action_drivevehicle", "hud_action_gogogo", "hud_action_healfriendly",
         "hud_action_mountMG", "hud_action_openbox", "hud_crosshair_overbuddy", "hud_bomb_frame", "hud_defuse_frame",
         "hud_vehicle_lifeframe"}


def ids(extract):
    index = json.load(open(os.path.join(extract, "index.json")))
    out = []
    for rid, info in index.items():
        name = info["names"][0]
        if name.lower().startswith("hud_") and "compass" not in name.lower() and rid not in DONE:
            out.append((rid, name))
    return sorted(out, key=lambda t: t[1].lower())


# --- curves ------------------------------------------------------------------------------------------------------

def rdp(p, eps):
    """a closed loop's points thinned to those that matter (Ramer-Douglas-Peucker, from its two farthest points)"""
    if len(p) < 4:
        return p
    i1 = int(np.argmax(((p - p[0]) ** 2).sum(1)))

    def run(q):
        if len(q) < 3:
            return list(q)
        a, b = q[0], q[-1]
        ab = b - a
        n = math.hypot(*ab)
        if n < 1e-9:
            d = np.hypot(*(q - a).T)
        else:
            d = np.abs(ab[0] * (q[:, 1] - a[1]) - ab[1] * (q[:, 0] - a[0])) / n
        k = int(np.argmax(d))
        if d[k] > eps and 0 < k < len(q) - 1:
            return run(q[:k + 1])[:-1] + run(q[k:])
        return [a, b]

    first = run(p[:i1 + 1])
    second = run(np.concatenate([p[i1:], p[:1]]))
    return np.array(first[:-1] + second[:-1])


def corner_smooth(p, iters=3, corner=50):
    """Chaikin's corner cutting, a corner sharper than `corner` degrees kept where it is"""
    for _ in range(iters):
        n = len(p)
        if n < 3:
            return p
        out = []
        for i in range(n):
            a, b, c = p[i - 1], p[i], p[(i + 1) % n]
            u, v = b - a, c - b
            nu, nv = math.hypot(*u), math.hypot(*v)
            turn = 0 if nu < 1e-9 or nv < 1e-9 else math.degrees(math.acos(max(-1, min(1, (u @ v) / (nu * nv)))))
            if turn > corner:
                out.append(b)
            else:
                out.append(a * 0.25 + b * 0.75)
                out.append(b * 0.75 + c * 0.25)
        p = np.array(out)
    return p


def loops_of(mask, level=0.5, U=6, soft=0.45, eps=0.22):
    """a mask's (0..1, the original's pixels) outlines as clean closed curves, in the original's coordinates"""
    h, w = mask.shape
    up = np.asarray(Image.fromarray(mask.astype(np.float32), "F").resize((w * U, h * U), Image.BILINEAR))
    up = blur(up, U * soft)
    out = []
    for l in contours(up, level):
        q = rdp(l / U, eps)
        if len(q) >= 3:
            out.append(corner_smooth(q))
    return out


def fill_cov(loops, w, h):
    """the loops filled (even-odd: a loop in another is a hole), antialiased, at K times"""
    S = K * SS
    img = np.zeros((h * K * SS, w * K * SS), np.uint8)
    for l in loops:
        m = Image.new("L", (w * K * SS, h * K * SS), 0)
        ImageDraw.Draw(m).polygon([(x * S, y * S) for x, y in l], fill=255)
        img ^= np.asarray(m)
    return np.asarray(Image.fromarray(img).resize((w * K, h * K), Image.BOX)).astype(np.float32) / 255


def stroke_cov(loops, w, h, width):
    S = K * SS
    m = Image.new("L", (w * K * SS, h * K * SS), 0)
    d = ImageDraw.Draw(m)
    for l in loops:
        pts = [(x * S, y * S) for x, y in l] + [(l[0][0] * S, l[0][1] * S)]
        d.line(pts, fill=255, width=max(1, int(S * width)), joint="curve")
    return np.asarray(m.resize((w * K, h * K), Image.BOX)).astype(np.float32) / 255


def over(out, cov, rgb, alpha):
    """a layer (cov 0..1, a colour) over out, straight alpha; alpha 0..1"""
    a2 = cov * alpha
    oa = out[..., 3] / 255
    na = a2 + oa * (1 - a2)
    rgb = np.asarray(rgb, np.float32)
    for k in range(3):
        out[..., k] = (rgb[k] * a2 + out[..., k] * oa * (1 - a2)) / np.maximum(na, 1e-6)
    out[..., 3] = na * 255
    return out


# --- masks -------------------------------------------------------------------------------------------------------

def fill_holes(m):
    h, w = m.shape
    out = np.zeros((h + 2, w + 2), bool)
    out[0, :] = out[-1, :] = out[:, 0] = out[:, -1] = True
    free = np.pad(~m, 1, constant_values=True)
    while True:
        n = out.copy()
        n[1:] |= out[:-1]
        n[:-1] |= out[1:]
        n[:, 1:] |= out[:, :-1]
        n[:, :-1] |= out[:, 1:]
        n &= free
        if (n == out).all():
            return ~out[1:-1, 1:-1]
        out = n


def grow(m, r):
    for _ in range(r):
        n = m.copy()
        n[1:] |= m[:-1]
        n[:-1] |= m[1:]
        n[:, 1:] |= m[:, :-1]
        n[:, :-1] |= m[:, 1:]
        m = n
    return m


def kmeans(x, k, seed=1):
    rng = np.random.default_rng(seed)
    c = [x[rng.integers(len(x))]]
    for _ in range(1, k):  # k-means++ seeds
        d = np.min([((x - q) ** 2).sum(1) for q in c], axis=0)
        if d.sum() <= 0:
            break
        c.append(x[rng.choice(len(x), p=d / d.sum())])
    c = np.array(c)
    for _ in range(30):
        lab = np.argmin(((x[:, None] - c[None]) ** 2).sum(2), 1)
        nc = np.array([x[lab == i].mean(0) if (lab == i).any() else c[i] for i in range(len(c))])
        if np.allclose(nc, c):
            break
        c = nc
    return lab, c


def tidy(lab, passes=2):
    """the dither's lone pixels (none of their four neighbours the same) given to most of their eight"""
    h, w = lab.shape
    for _ in range(passes):
        p = np.pad(lab, 1, constant_values=-2)
        four = np.stack([p[0:h, 1:w + 1], p[2:h + 2, 1:w + 1], p[1:h + 1, 0:w], p[1:h + 1, 2:w + 2]])
        lone = ~(four == lab).any(0)
        eight = np.stack([p[y:y + h, x:x + w] for y in range(3) for x in range(3) if (y, x) != (1, 1)])
        new = lab.copy()
        for yy, xx in zip(*np.where(lone)):
            vals, cnt = np.unique(eight[:, yy, xx], return_counts=True)
            ok = vals != -2
            if ok.any():
                new[yy, xx] = vals[ok][np.argmax(cnt[ok])]
        lab = new
    return lab


def measure(src, sel):
    a = src[..., 3] / 255.0
    rgb = src[..., :3].astype(np.float32)
    if not sel.any():
        return np.zeros(3, np.float32), 0.0
    wgt = a[sel]
    return (rgb[sel] * wgt[:, None]).sum(0) / max(wgt.sum(), 1e-6), float(wgt.mean())


# --- the icons ---------------------------------------------------------------------------------------------------

def rrect_cov(x0, y0, x1, y1, r, w, h):
    """a rounded rectangle (the original's coordinates) filled, antialiased, at K times"""
    S = K * SS
    m = Image.new("L", (w * K * SS, h * K * SS), 0)
    if x1 > x0 and y1 > y0:
        ImageDraw.Draw(m).rounded_rectangle([x0 * S, y0 * S, x1 * S, y1 * S], radius=max(0, r) * S, fill=255)
    return np.asarray(m.resize((w * K, h * K), Image.BOX)).astype(np.float32) / 255


def frame_fit(a):
    """an icon's rectangular frame (a band of even width all round its drawing): its outer box, width and corner
    radius in the original's pixels, or None"""
    m = a > 0.45
    if m.sum() < 20:
        return None
    ys, xs = np.nonzero(m)
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    # (a bar's frame holds icons and dividers at its ends: traced with them)
    if x1 - x0 < 12 or y1 - y0 < 12 or (x1 - x0) > 2.5 * (y1 - y0):
        return None

    def runs(lines):
        out = []
        for v in lines:
            k = 0
            while k < len(v) and v[k]:
                k += 1
            out.append(k)
        return out

    hy = slice(y0 + (y1 - y0) // 4, y1 - (y1 - y0) // 4)
    hx = slice(x0 + (x1 - x0) // 4, x1 - (x1 - x0) // 4)
    sides = [runs(m[hy, x0:x1]), runs(m[hy, x0:x1][:, ::-1]), runs(m[y0:y1, hx].T), runs(m[y0:y1, hx][::-1].T)]
    t = int(min(np.median(r) for r in sides))
    if t < 1 or t > 6 or min(np.mean(np.array(r) >= t) for r in sides) < 0.7:
        return None
    # the corner radius whose band matches the drawing best at the corners
    yy, xx = np.mgrid[0:a.shape[0], 0:a.shape[1]] + 0.5
    best = (-1.0, 0.0)
    for r in np.arange(0, min(x1 - x0, y1 - y0) / 3, 0.5):
        cov = np.zeros(a.shape, bool)
        # (a pixel's middle inside the outer rounded box and outside the inner one)
        def inside(bx0, by0, bx1, by1, rr):
            qx = np.maximum(np.maximum(bx0 + rr - xx, xx - (bx1 - rr)), 0)
            qy = np.maximum(np.maximum(by0 + rr - yy, yy - (by1 - rr)), 0)
            return (np.hypot(qx, qy) <= rr) & (xx > bx0) & (xx < bx1) & (yy > by0) & (yy < by1)
        band = inside(x0, y0, x1, y1, r) & ~inside(x0 + t, y0 + t, x1 - t, y1 - t, max(r - t, 0))
        corner = ((xx < x0 + 2 * t + r) | (xx > x1 - 2 * t - r)) & ((yy < y0 + 2 * t + r) | (yy > y1 - 2 * t - r))
        iou = (band & m & corner).sum() / max(((band | m) & corner & ((xx < x0 + t + r) | (xx > x1 - t - r) | (yy < y0 + t + r) | (yy > y1 - t - r))).sum(), 1)
        best = max(best, (iou, r))
    return float(x0), float(y0), float(x1), float(y1), float(t), best[1]


def frame_layer(a, src, fr, w, h):
    """the frame as a layer (cov, colour, alpha) and the original's alpha with the frame's band taken out"""
    x0, y0, x1, y1, t, r = fr
    cov = np.clip(rrect_cov(x0, y0, x1, y1, r, w, h) - rrect_cov(x0 + t, y0 + t, x1 - t, y1 - t, max(r - t, 0), w, h), 0, 1)
    yy, xx = np.mgrid[0:h, 0:w] + 0.5
    band = (xx < x0 + t) | (xx > x1 - t) | (yy < y0 + t) | (yy > y1 - t)
    col, al = measure(src, band & (a > 0.45))
    rest = a.copy()
    near = (xx < x0 + t + 0.6) | (xx > x1 - t - 0.6) | (yy < y0 + t + 0.6) | (yy > y1 - t - 0.6)
    rest[near] = 0
    return cov, col, al, rest


def alpha_levels(a):
    """the alpha's tones (0..1): its values grouped, the dither's in between taken as the tone they make on average"""
    drawn = a > 0.06
    vals = blur(a, 0.6)[drawn]
    lab, cent = kmeans(vals[:, None], min(3, len(np.unique(np.round(vals * 20)))))
    cent = sorted(float(c[0]) for i, c in enumerate(cent) if (lab == i).sum() > max(3, 0.02 * len(vals)))
    out = []
    for c in cent:  # tones closer than 0.12 are one
        if out and c - out[-1] < 0.12:
            out[-1] = (out[-1] + c) / 2
        else:
            out.append(c)
    return out


def mono(src):
    """one colour drawn in its alpha: each tone's regions (the dither smoothed into the tone it makes) traced and
    stacked, the more opaque over the less"""
    h, w = src.shape[:2]
    a = src[..., 3] / 255.0
    col, _ = measure(src, a > 0.1)
    lv = alpha_levels(a)
    fr = frame_fit(a)
    inner = a
    if fr:
        fcov, _, fal, inner = frame_layer(a, src, fr, w, h)
    out = np.zeros((h * K, w * K, 4), np.float32)
    alpha = np.zeros((h * K, w * K), np.float32)
    prev = 0.0
    sm = blur(inner, 0.45)  # (a dithered tone even, a line of a pixel kept)
    for L in lv:
        cov = fill_cov(loops_of(sm, level=(prev + L) / 2, soft=0.3, eps=0.3), w, h)
        alpha += cov * (L - prev)
        prev = L
    if fr:
        alpha = np.maximum(alpha, fcov * fal)
    out[..., :3] = col
    out[..., 3] = np.clip(alpha, 0, 1) * 255
    return out, "mono tones " + " ".join("%.2f" % L for L in lv) + (" frame %.0f radius %.1f" % (fr[4], fr[5]) if fr else "")


def toned(src):
    """one hue in a few shades: the image smoothed (the dither into the tone it makes), its pixels grouped by brightness
    and opacity into a few tones, each tone's regions traced and filled flat, the see-through and darker first"""
    h, w = src.shape[:2]
    a0 = src[..., 3] / 255.0
    fr = frame_fit(a0)
    a = a0
    if fr:
        fcov, fcol, fal, a = frame_layer(a0, src, fr, w, h)
    rgb = src[..., :3].astype(np.float32)
    pm = np.stack([blur(rgb[..., c] * a, 0.45) for c in range(3)], -1)
    sa = blur(a, 0.45)
    drawn = sa > 0.08
    lum = (pm * [0.3, 0.59, 0.11]).sum(2) / np.maximum(sa, 1e-3)
    feats = np.stack([lum[drawn] / 255, sa[drawn] * 1.3], 1)
    k = min(4, len(np.unique(np.round(feats * 12), axis=0)))
    lab_d, cent = kmeans(feats, k)
    lab = np.full(a.shape, -1)
    lab[drawn] = lab_d
    lab = tidy(lab)
    layers = []
    for i in range(len(cent)):
        m = lab == i
        if m.sum() < 2:
            continue
        col, al = measure(src, m & (a > 0.05)) if (m & (a > 0.05)).any() else (np.zeros(3), 0.0)
        layers.append((al, float((np.asarray(col) * [0.3, 0.59, 0.11]).sum()), i, col))
    layers.sort(key=lambda t: (round(t[0], 1), t[1]))
    out = np.zeros((h * K, w * K, 4), np.float32)
    for al, _, i, col in layers:
        cov = fill_cov(loops_of((lab == i).astype(np.float32), level=0.42, soft=0.35, eps=0.3), w, h)
        out = over(out, cov, col, al)
    if fr:
        out = over(out, fcov, fcol, fal)
    return out, "toned %d" % len(layers) + (" frame %.0f radius %.1f" % (fr[4], fr[5]) if fr else "")


def painted(src, ai):
    """a painted object: the pack's AI colour for its detail inside, its silhouette traced clean, its soft shadow"""
    h, w = src.shape[:2]
    W, H = w * K, h * K
    a = src[..., 3] / 255.0
    sil = fill_cov(loops_of(a, level=0.5, soft=0.35, eps=0.18), w, h)
    # the soft parts outside the silhouette (a shadow, a glow) as they fade, smooth
    soft = np.clip(np.asarray(Image.fromarray(a.astype(np.float32), "F").resize((W, H), Image.BICUBIC)), 0, 1)
    soft = blur(np.minimum(soft, 0.5), K * 0.4)
    inside = float(np.percentile(a[a > 0.5], 50)) if (a > 0.5).any() else 1.0
    alpha = np.maximum(sil * inside, soft * (1 - sil))
    # the colour: the original's spread past its edge (no dark fringe), the AI's detail well inside
    rgb = src[..., :3].astype(np.float32)
    known = a > 0.02
    for _ in range(6):
        p = np.pad(rgb, ((1, 1), (1, 1), (0, 0)), mode="edge")
        kk = np.pad(known, 1).astype(np.float32)
        acc = sum(p[y:y + h, x:x + w] * kk[y:y + h, x:x + w, None] for y in range(3) for x in range(3))
        cnt = sum(kk[y:y + h, x:x + w] for y in range(3) for x in range(3))
        g = (~known) & (cnt > 0)
        rgb[g] = acc[g] / cnt[g][:, None]
        known |= g
    col = np.stack([np.asarray(Image.fromarray(rgb[..., c], "F").resize((W, H), Image.BICUBIC)) for c in range(3)], -1)
    if ai is not None:
        core = blur(sil, K * 0.5)
        mask = np.clip((core - 0.6) * 4, 0, 1)
        col = col * (1 - mask[..., None]) + ai[..., :3] * mask[..., None]
    return np.concatenate([np.clip(col, 0, 255), alpha[..., None] * 255], -1), "painted"


def outlined(src):
    """a tank's or a buddy's outline: the silhouette traced whole, a line along it, a see-through fill, a halo"""
    h, w = src.shape[:2]
    a = src[..., 3] / 255.0
    top = float(np.percentile(a[a > 0.04], 97))
    line = a >= 0.8 * top
    sil = fill_holes(grow(line, 1))
    sil &= ~(grow(~sil, 1) & ~line)
    inner = sil & ~grow(~sil, 2)
    ring_px = grow(sil, 2) & ~sil & (a > 0.01)
    line_c, line_a = measure(src, line)
    fill_c, fill_a = measure(src, inner & ~line)
    halo_c, halo_a = measure(src, ring_px)
    loops = loops_of(sil.astype(np.float32), level=0.5, U=8, soft=0.28, eps=0.15)
    body = fill_cov(loops, w, h)
    stroke = stroke_cov(loops, w, h, 0.95)
    halo = np.clip(blur(body, K * 1.0) * 1.3 - body * 0.3, 0, 1)
    # the drawing inside it (a turret's hatches, a hull's line, the wheels): the line's pixels away from the edge
    det = line & ~grow(~sil, 1)
    if det.sum() > 0.35 * max(inner.sum(), 1):  # (a light dithered inside: the fill, its average)
        fill_c, fill_a = measure(src, inner)
        det = np.zeros_like(det)
    detail = fill_cov(loops_of(blur(det.astype(np.float32), 0.35), level=0.3, U=8, soft=0.2, eps=0.12), w, h) if det.sum() > 2 else 0
    out = np.zeros((h * K, w * K, 4), np.float32)
    out = over(out, halo, halo_c, halo_a)
    out = over(out, body, fill_c, fill_a)
    out = over(out, stroke, line_c, line_a)
    if det.sum() > 2:
        out = over(out, detail, line_c, line_a)
    return out, "outlined line %.2f fill %.2f halo %.2f" % (line_a, fill_a, halo_a)


def tank_tones(src):
    """the tank icons' greys and opacities: the line's, the fill's, the halo's"""
    a = src[..., 3] / 255.0
    top = float(np.percentile(a[a > 0.04], 97))
    line = a >= 0.8 * top
    mid = (a > 0.3 * top) & ~line
    soft = (a > 0.02) & (a <= 0.3 * top)
    return measure(src, line), measure(src, mid), measure(src, soft)


def halo_of(cov, w, h):
    return np.clip(blur(cov, K * 1.1) * 1.4 - cov * 0.4, 0, 1)


def t34_body(src):
    """the T34's hull seen from above, drawn anew: a plate with a track down each side (their treads), its front and
    back edges, the arrow ahead of it; a see-through grey with light lines, a soft dark halo (the original's place:
    20..47 by 17..54, the arrow from 8)"""
    h, w = src.shape[:2]
    (lc, la), (fc, fa), (hc, ha) = tank_tones(src)
    plate, lines, treads = Canvas(w, h), Canvas(w, h), Canvas(w, h)
    x0, x1, y0, y1 = 20.0, 47.0, 17.0, 54.0
    tw = 5.4  # a track's width
    plate.rect(x0, y0, x1, y1, r=1.2)
    plate.poly([(33.5, 8.2), (39.6, 15.6), (27.4, 15.6)])
    L = 0.95  # a line's width
    # the hull's outline, the tracks' inner edges, the deck's front and back
    lines.rect(x0, y0, x1, y1, r=1.2)
    lines.d.rounded_rectangle(lines.pts([(x0 + L, y0 + L), (x1 - L, y1 - L)]), radius=0.4 * K * SS, fill=0)
    for x in (x0 + tw, x1 - tw - L):
        lines.rect(x, y0 + 1.2, x + L * 0.8, y1 - 1.2)
    lines.rect(x0 + tw, y0 + 1.0, x1 - tw, y0 + 1.0 + L)
    lines.rect(x0 + tw, y1 - 1.0 - L, x1 - tw, y1 - 1.0)
    # the arrow: a light triangle with a darker inside
    lines.poly([(33.5, 8.2), (39.6, 15.6), (27.4, 15.6)])
    arrow_in = Canvas(w, h)
    arrow_in.poly([(33.5, 10.4), (37.4, 14.6), (29.6, 14.6)])
    # the treads: short bars across each track
    for y in np.arange(y0 + 2.6, y1 - 2.0, 2.2):
        for xa in (x0 + 1.4, x1 - tw + 0.6):
            treads.rect(xa, y, xa + tw - 2.0, y + 0.7, r=0.2)
    pc, lc_, ai, tc = plate.coverage(), lines.coverage(), arrow_in.coverage(), treads.coverage()
    out = np.zeros((h * K, w * K, 4), np.float32)
    out = over(out, halo_of(pc, w, h), hc, max(ha, 0.2))
    out = over(out, pc, fc, fa)
    out = over(out, tc, lc, la * 0.55)
    out = over(out, np.clip(lc_ - ai, 0, 1), lc, la)
    out = over(out, ai, fc, fa)
    return out, "drawn: T34 hull"


def t34_turret(src):
    """the T34's turret seen from above, drawn anew: the cast turret (an egg, wider at the back), the gun's mantlet,
    the barrel to its muzzle, the commander's hatch; a solid light grey with a lighter rim (the original's
    place: the barrel from 6, the turret 28..57, its middle at 17)"""
    h, w = src.shape[:2]
    (lc, la), (fc, fa), (hc, ha) = tank_tones(src)
    cx = 17.0
    body, rim, dark, barrel = Canvas(w, h), Canvas(w, h), Canvas(w, h), Canvas(w, h)
    # the turret: a superellipse egg, its front (top) narrower
    pts = []
    for t in np.linspace(0, 2 * np.pi, 160, endpoint=False):
        c, s_ = np.cos(t), np.sin(t)
        ry = 14.0
        rx = 8.4 + 1.0 * s_  # wider at the back (down)
        x = cx + rx * np.sign(c) * abs(c) ** 0.8
        y = 43.2 + ry * np.sign(s_) * abs(s_) ** 0.9
        pts.append((x, y))
    body.poly(pts)
    inner = [(cx + (x - cx) * 0.9, 43.4 + (y - 43.2) * 0.93) for x, y in pts]
    rim.poly(pts)
    rim.d.polygon(rim.pts(inner), fill=0)
    # the barrel and its muzzle, the mantlet where it meets the turret
    barrel.rect(cx - 1.25, 6.4, cx + 1.25, 31.0, r=0.5)
    barrel.rect(cx - 1.7, 5.6, cx + 1.7, 8.4, r=0.5)
    barrel.rect(cx - 3.6, 27.6, cx + 3.6, 32.0, r=1.0)
    # the hatch and the vision blocks, darker
    dark.d.ellipse(dark.pts([(cx - 3.4, 44.0), (cx + 3.4, 50.8)]), fill=255)
    dark.rect(cx - 6.2, 36.0, cx - 4.6, 38.4, r=0.4)
    dark.rect(cx + 4.6, 36.0, cx + 6.2, 38.4, r=0.4)
    hatch_rim = Canvas(w, h)
    hatch_rim.d.ellipse(hatch_rim.pts([(cx - 3.4, 44.0), (cx + 3.4, 50.8)]), outline=255, width=int(0.8 * K * SS))
    bc, rc, dc, brc, hr = body.coverage(), rim.coverage(), dark.coverage(), barrel.coverage(), hatch_rim.coverage()
    # (no halo: the game draws the turret cut out by its alpha, a soft halo came out as a ragged dark edge)
    out = np.zeros((h * K, w * K, 4), np.float32)
    # (solid, as the original's: a light grey a little darker than its rim)
    solid = np.asarray(lc, np.float32) * 0.82
    out = over(out, bc, solid, la)
    out = over(out, brc, solid, la)
    out = over(out, dc, hc, 0.55)
    out = over(out, np.maximum(rc, hr), lc, la)
    edge = Canvas(w, h)  # the barrel's light edge
    edge.rect(cx - 1.25, 6.4, cx - 0.55, 27.6)
    out = over(out, edge.coverage(), lc, la)
    return out, "drawn: T34 turret"


def lifeframe(src):
    """the tank's armour and health frame drawn anew (its places as the original's: the plate 0..128 by 1..30, the
    two bars' wells 2..111 by 3..13 and 15..28, the icons' panel from 111): a light grey plate a little bevelled, the
    wells see-through dark with a shade along their top, a shield (the armour) and a cross (the health) on the panel"""
    h, w = src.shape[:2]
    a = src[..., 3] / 255.0
    plate_c, plate_a = measure(src, a > 0.9)
    well_c, well_a = measure(src, (a > 0.2) & (a < 0.45))
    cross_c, cross_a = measure(src, (a > 0.5) & (a < 0.7))
    plate, wells, shine, shade = Canvas(w, h), Canvas(w, h), Canvas(w, h), Canvas(w, h)
    plate.rect(0.0, 1.0, 128.0, 30.0, r=1.4)
    for y0, y1 in ((3.0, 13.0), (15.0, 28.0)):
        wells.rect(2.0, y0, 111.0, y1, r=0.8)
        shade.rect(2.0, y0, 111.0, y0 + 1.6, r=0.8)
    shine.rect(0.6, 1.2, 127.4, 1.9, r=0.4)
    pc = plate.coverage()
    wc = wells.coverage()
    out = np.zeros((h * K, w * K, 4), np.float32)
    out = over(out, np.clip(pc - wc, 0, 1), plate_c, plate_a)
    out = over(out, wc, well_c, well_a)
    out = over(out, shade.coverage() * wc, (0, 0, 0), 0.35)
    out = over(out, shine.coverage(), np.minimum(np.asarray(plate_c) * 1.25, 255), 0.6)
    lower = Canvas(w, h)  # the plate's lower edge a touch darker
    lower.rect(0.6, 29.1, 127.4, 29.8, r=0.4)
    out = over(out, lower.coverage(), np.asarray(plate_c) * 0.7, 0.6)
    # the armour: a shield, cut out of the panel (see-through dark as the wells)
    sh = Canvas(w, h)
    cx = 119.5
    pts = [(cx - 5.2, 4.0), (cx + 5.2, 4.0), (cx + 5.0, 7.0)]
    for t in np.linspace(0, 1, 24):  # its sides curving in to the point
        pts.append((cx + 5.0 * (1 - t) ** 1.4, 7.0 + 4.6 * t))
    pts += [(cx - 5.0 * (1 - t) ** 1.4, 7.0 + 4.6 * t) for t in np.linspace(1, 0, 24)]
    pts.append((cx - 5.0, 7.0))
    sh.poly(pts)
    out = over(out, sh.coverage(), well_c, 1.0)
    out = over(out, sh.coverage(), well_c, 0)  # (keeps the panel's colour about it)
    out[..., 3] = np.where(sh.coverage() > 0, out[..., 3] * (1 - sh.coverage()) + sh.coverage() * well_a * 255, out[..., 3])
    # the health: a cross, rounded, see-through as the original's
    cr = Canvas(w, h)
    cr.rect(cx - 2.0, 15.0, cx + 2.0, 27.0, r=0.7)
    cr.rect(cx - 6.0, 19.0, cx + 6.0, 23.0, r=0.7)
    cc = cr.coverage()
    out[..., :3] = out[..., :3] * (1 - cc[..., None]) + np.asarray(cross_c, np.float32) * cc[..., None]
    out[..., 3] = out[..., 3] * (1 - cc) + cc * cross_a * 255
    return out, "drawn: armour and health frame"


def ring(src):
    """the tank's aiming ring: a true circle fitted to the original's line, its dark inside, its halo"""
    h, w = src.shape[:2]
    a = src[..., 3] / 255.0
    top = float(np.percentile(a[a > 0.04], 97))
    line = a >= 0.8 * top
    ys, xs = np.nonzero(line)
    x, y = xs + 0.5, ys + 0.5
    A = np.stack([x, y, np.ones_like(x)], 1)
    sol = np.linalg.lstsq(A, x * x + y * y, rcond=None)[0]
    cx, cy = sol[0] / 2, sol[1] / 2
    r = math.sqrt(sol[2] + cx * cx + cy * cy)
    yy, xx = np.mgrid[0:h, 0:w] + 0.5
    d = np.hypot(xx - cx, yy - cy)
    line_c, line_a = measure(src, line)
    fill_c, fill_a = measure(src, (d < r - 1.5) & (a > 0.02))
    halo_c, halo_a = measure(src, (d > r + 1) & (d < r + 3) & (a > 0.01))
    W, H = w * K, h * K
    S = K * SS

    def disc(rad):
        m = Image.new("L", (W * SS, H * SS), 0)
        ImageDraw.Draw(m).ellipse([(cx - rad) * S, (cy - rad) * S, (cx + rad) * S, (cy + rad) * S], fill=255)
        return np.asarray(m.resize((W, H), Image.BOX)).astype(np.float32) / 255

    body = disc(r)
    stroke = np.clip(disc(r + 0.45) - disc(r - 0.45), 0, 1)
    halo = blur(np.clip(disc(r + 2.2) - body, 0, 1), K * 0.6)
    out = np.zeros((H, W, 4), np.float32)
    out = over(out, halo, halo_c, halo_a)
    out = over(out, body, fill_c, fill_a)
    out = over(out, stroke, line_c, line_a)
    return out, "ring at %.2f %.2f radius %.2f" % (cx, cy, r)


def main():
    extract, pack = sys.argv[1], sys.argv[2]
    work = os.path.join(pack, "work_hud2")
    os.makedirs(work, exist_ok=True)
    only = set(sys.argv[3].split(",")) if len(sys.argv) > 3 else None
    files = []
    for rid, name in ids(extract):
        if only and rid not in only and name not in only:
            continue
        src = np.asarray(Image.open(os.path.join(extract, rid + ".png")).convert("RGBA")).astype(np.float32)
        if name in RING:
            out, kind = ring(src)
        elif name == "hud_vehicle_lifeframe":
            out, kind = lifeframe(src)
        elif name == "hud_t34_body":
            out, kind = t34_body(src)
        elif name == "hud_t34_turret":
            out, kind = t34_turret(src)
        elif name in OUTLINED:
            out, kind = outlined(src)
        elif name in TONED:
            out, kind = toned(src)
        else:
            drawn = src[..., 3] > 25
            if src[..., :3][drawn].std(0).mean() < 10:
                out, kind = mono(src)
            else:
                kept = os.path.join(pack, "work_hud2_src", rid + ".png")
                h, w = src.shape[:2]
                ai = (np.asarray(Image.open(kept).convert("RGBA").resize((w * K, h * K), Image.LANCZOS)).astype(np.float32)
                      if os.path.exists(kept) else None)
                out, kind = painted(src, ai)
        p = os.path.join(work, rid + ".c.png")
        Image.fromarray((np.clip(out, 0, 255) + 0.5).astype(np.uint8), "RGBA").save(p)
        files.append(p)
        print(rid, name, kind, flush=True)
    generate.texconv(files, "BC7_UNORM", os.path.join(pack, "textures"))


if __name__ == "__main__":
    main()
