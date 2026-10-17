"""Every piece of the HUD's texture dictionary drawn again at up to 512 pixels, as the compass was (hud.py): crisp
edges filtered down from twice the size, BC7 with mips, plain ui textures in materials.txt.

The pieces are small flat pictures (the action icons' green line art, the postures, crosshairs, frames, the tank
outlines) or small paintings (grenades, the medkit, flags, medals). Their shapes are rebuilt the same way: a
coverage (the alpha, or for flat art each of its few colours) upscaled smoothly reads as a distance field, its
half level is the edge, and each pixel takes the edge at exactly one pixel's width, so steps and blur both go and
lines keep their width. Flat art keeps its colours flat between those edges; a painting keeps the pack's upscaled
colour (pack/work/<id>.c.png) inside its rebuilt outline. Soft parts (glows, glass) stay soft.

    python -I hud_icons.py <pack folder> <extract folder> [names...]   (names: only those, e.g. hud_ammo)
"""
import json
import os
import sys

import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate  # noqa: E402

# the HUD's dictionary (tools/remaster/txd_list.py COD.PAK hud_compass_bg): drawn by hud.py already
DONE = {"hud_compass_bg", "hud_compass_glass", "hud_compass_star", "hud_compass_star_pulse", "hud_compass_star_arrow",
        "hud_compass_arrow", "hud_crosshair"}
# the paintings: colour from the AI upscale
PAINTED = ("hud_inventory_", "Objective_", "hud_allied_objective", "hud_german_objective", "hud_bomb_location",
           "hud_bomb_placed", "hud_ctf_flag", "hud_ctf_american", "hud_ctf_british", "hud_ctf_german",
           "hud_ctf_soviet", "hud_action_bomb",
           # line art: fine strokes the posterizing would lose
           "hud_posture_", "hud_person_buddy_health", "hud_M12_buddy_health", "hud_T34_buddy_health", "hud_t34_body",
           "hud_t34_turret", "hud_sherman_turret")
MAX = 512


def painted(name):
    return name != "hud_inventory" and any(name.startswith(p) for p in PAINTED)


def blur(f, sigma):
    """separable gaussian, edges clamped"""
    r = max(1, int(sigma * 3))
    x = np.arange(-r, r + 1, dtype=np.float32)
    k = np.exp(-x * x / (2 * sigma * sigma))
    k /= k.sum()
    p = np.pad(f, ((0, 0), (r, r)), mode="edge")
    f = sum(k[i] * p[:, i:i + f.shape[1]] for i in range(len(k)))
    p = np.pad(f, ((r, r), (0, 0)), mode="edge")
    return sum(k[i] * p[i:i + f.shape[0], :] for i in range(len(k)))


def up(a, w, h):
    """a float field upscaled smoothly (bicubic), then rid of the source's pixel steps"""
    im = Image.fromarray(a.astype(np.float32), "F").resize((w, h), Image.BICUBIC)
    k = w / a.shape[1]
    return blur(np.asarray(im, np.float32), 0.42 * k)


def edge(f, level=0.5):
    """coverage of f > level, one pixel wide at its edge: (f - level) / |grad f| is the distance in pixels"""
    gy, gx = np.gradient(f)
    g = np.sqrt(gx * gx + gy * gy) + 1e-6
    return np.clip((f - level) / g + 0.5, 0, 1)


def local_max(a, r):
    """the largest within r pixels (a square, separably)"""
    p = np.pad(a, ((0, 0), (r, r)), mode="edge")
    out = a.copy()
    for d in range(2 * r + 1):
        out = np.maximum(out, p[:, d:d + a.shape[1]])
    p = np.pad(out, ((r, r), (0, 0)), mode="edge")
    for d in range(2 * r + 1):
        out = np.maximum(out, p[d:d + a.shape[0], :])
    return out


def kmeans(px, k_max=7, merge=0.06):
    """the flat art's colours (premultiplied RGBA): k-means, close ones merged; transparency is one of them"""
    rng = np.random.default_rng(1)
    centres = np.vstack([np.zeros((1, 4), np.float32), px[rng.choice(len(px), min(k_max - 1, len(px)), replace=False)]])
    for _ in range(30):
        d = ((px[:, None, :] - centres[None]) ** 2).sum(-1)
        lab = d.argmin(1)
        centres = np.array([px[lab == i].mean(0) if (lab == i).any() else centres[i] for i in range(len(centres))])
    keep = []
    for c in centres:
        if all(np.abs(c - o).max() > merge for o in keep):
            keep.append(c)
    return np.array(keep, np.float32)


def sharpness(f):
    """per pixel, the largest step to a neighbour"""
    g = np.zeros_like(f)
    p = np.pad(f, 1, mode="edge")
    for dy, dx in ((0, 1), (2, 1), (1, 0), (1, 2)):
        g = np.maximum(g, np.abs(f - p[dy:dy + f.shape[0], dx:dx + f.shape[1]]))
    return g


def shape(f, r):
    """a coverage field's edges rebuilt: its half level relative to its own local peak, so thin and faint lines keep
    their width and soft fills their level"""
    lm = np.maximum(local_max(f, r), 1e-3)
    return edge(f / lm, 0.5) * lm


def bezier(p0, p1, p2, p3, n=12):
    t = np.linspace(0, 1, n + 1)[1:, None]
    return ((1 - t) ** 3) * p0 + 3 * ((1 - t) ** 2) * t * p1 + 3 * (1 - t) * t * t * p2 + t ** 3 * p3


def vector(mask, out_w, out_h, k):
    """a mask (k times the source's size) traced to curves (potrace) and filled anti-aliased at out_w x out_h"""
    import potrace
    from PIL import ImageDraw
    ss = 4
    W, H = out_w * ss, out_h * ss
    f = W / mask.shape[1]
    acc = np.zeros((H, W), bool)
    path = potrace.Bitmap(~mask).trace(turdsize=k * k // 4, alphamax=1.1, opticurve=True, opttolerance=0.45)
    for curve in path.curves:
        pts = [np.array([curve.start_point.x, curve.start_point.y])]
        for seg in curve.segments:
            end = np.array([seg.end_point.x, seg.end_point.y])
            if seg.is_corner:
                pts += [np.array([seg.c.x, seg.c.y]), end]
            else:
                pts += list(bezier(pts[-1], np.array([seg.c1.x, seg.c1.y]), np.array([seg.c2.x, seg.c2.y]), end))
        poly = Image.new("1", (W, H), 0)
        ImageDraw.Draw(poly).polygon([(float(x) * f, float(y) * f) for x, y in pts], fill=1)
        acc ^= np.asarray(poly, bool)  # even-odd: holes come out
    return np.asarray(Image.fromarray(acc.astype(np.uint8) * 255).resize((out_w, out_h), Image.BOX), np.float32) / 255


def rebuild(src, name, ai, paint):
    """src: RGBA 0..1 at the original size; ai: the pack's upscale (RGBA 0..1) or None; paint: a painting"""
    h, w = src.shape[:2]
    scale = min(8, MAX // max(w, h))
    ow, oh = w * scale, h * scale
    W, H = ow * 2, oh * 2  # drawn at twice, filtered down
    # the shapes are traced from the pack's upscale when there is one (its four times more of the drawing), at
    # twice its size; else from the original at four times
    g = ai if ai is not None else src
    gh, gw = g.shape[:2]
    K = 2 if ai is not None else 4
    pmg = np.dstack([g[..., :3] * g[..., 3:], g[..., 3]])
    a = src[..., 3]
    pm = np.dstack([src[..., :3] * a[..., None], a])
    # where the picture steps sharply (edges, lines) its shapes are vectors; where it eases (glows, shading) it stays
    lum = pm[..., :3] @ np.array([0.3, 0.59, 0.11], np.float32)
    crisp = np.clip((np.maximum(sharpness(a), sharpness(lum)) - 0.1) / 0.2, 0, 1)
    crisp = np.clip(np.asarray(Image.fromarray(local_max(crisp, 2), "F").resize((W, H), Image.BILINEAR), np.float32), 0, 1)
    smooth = np.dstack([np.clip(up(pm[..., i], W, H), 0, 1) for i in range(4)])
    def mask_of(field):
        f = np.asarray(Image.fromarray(blur(field.astype(np.float32), 0.8), "F").resize((gw * K, gh * K), Image.BICUBIC), np.float32)
        return f > 0.5
    if paint and ai is not None:
        ga = g[..., 3]
        alpha_v = vector(mask_of(ga / max(ga.max(), 1e-3)), W, H, K * gw // w) * np.clip(local_max(smooth[..., 3], scale * 2), 0, 1)
        alpha = smooth[..., 3] + (alpha_v - smooth[..., 3]) * crisp
        col = np.asarray(Image.fromarray((ai[..., :3] * 255).astype(np.uint8)).resize((W, H), Image.LANCZOS), np.float32) / 255
        out = np.dstack([col * alpha[..., None], alpha])
    else:
        # the colours from the inside of the shapes: an edge's anti-aliased pixels are blends, not colours of their own
        steps = np.max(np.stack([sharpness(pmg[..., i]) for i in range(4)]), 0)
        core = pmg[steps < (0.06 if ai is not None else 0.12)]
        cent = kmeans(core if len(core) > 16 else pmg.reshape(-1, 4), merge=0.1)
        d = ((pmg[..., None, :] - cent[None, None]) ** 2).sum(-1)
        m = np.exp(-(d - d.min(-1, keepdims=True)) / 0.003)
        m /= m.sum(-1, keepdims=True)
        # each colour's region, at sub-pixel precision: the largest share of the smooth upscale
        ups = np.stack([np.asarray(Image.fromarray(blur(m[..., i].astype(np.float32), 0.8 if ai is not None else 0.4), "F").resize((gw * K, gh * K), Image.BICUBIC), np.float32)
                        for i in range(len(cent))], -1)
        label = ups.argmax(-1)
        flat = np.zeros((H, W, 4), np.float32)
        # the regions as vectors, the largest first, each grown a little under the next (no seams)
        order = sorted(range(len(cent)), key=lambda i: -(label == i).sum())
        for n, i in enumerate(order):
            reg = label == i
            if n == 0:
                cov = np.ones((H, W), np.float32)
            else:
                if reg.sum() == 0:
                    continue
                grown = reg.copy()
                grown[1:] |= reg[:-1]; grown[:-1] |= reg[1:]; grown[:, 1:] |= reg[:, :-1]; grown[:, :-1] |= reg[:, 1:]
                cov = vector(grown, W, H, K * gw // w)
            flat = flat * (1 - cov[..., None]) + cent[i][None, None] * cov[..., None]
        out = smooth + (flat - smooth) * crisp[..., None]
    out = np.clip(out, 0, 1)
    out[..., :3] = np.minimum(out[..., :3], out[..., 3:])
    # filtered down premultiplied, then straight colour; transparent pixels take their neighbours' (the mips and
    # the bilinear filter blend into it)
    small = np.asarray(Image.fromarray((out * 255 + 0.5).astype(np.uint8), "RGBA").resize((ow, oh), Image.LANCZOS), np.float32) / 255
    rgb = np.where(small[..., 3:] > 1e-3, small[..., :3] / np.maximum(small[..., 3:], 1e-3), 0)
    fill = np.asarray(Image.fromarray((np.clip(rgb, 0, 1) * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(5)), np.float32) / 255
    rgb = np.where(small[..., 3:] > 0.02, rgb, fill)
    res = np.dstack([np.clip(rgb, 0, 1), np.clip(small[..., 3], 0, 1)])
    if name.startswith("hud_inventory_"):
        res[..., 3] *= joined(res[..., 3])
    return (res * 255 + 0.5).astype(np.uint8)


def joined(a, level=0.05, share=0.02):
    """1 on the pieces of the picture (coverage over level, 8-connected) at least share of the largest, 0 on the specks
    left apart from it (the medkit's shadow's crumbs under it, floating over the ammo's frame)"""
    from collections import deque
    on = a > level
    h, w = on.shape
    label = np.zeros((h, w), np.int32)
    sizes = [0]
    for y0, x0 in zip(*np.nonzero(on)):
        if label[y0, x0]:
            continue
        n = len(sizes)
        label[y0, x0] = n
        q, size = deque([(y0, x0)]), 0
        while q:
            y, x = q.popleft()
            size += 1
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    yy, xx = y + dy, x + dx
                    if 0 <= yy < h and 0 <= xx < w and on[yy, xx] and not label[yy, xx]:
                        label[yy, xx] = n
                        q.append((yy, xx))
        sizes.append(size)
    if len(sizes) <= 2:
        return np.ones((h, w), np.float32)
    keep = np.array(sizes) >= max(sizes) * share
    keep[0] = True  # below the level: left as it is (the soft edges)
    return keep[label].astype(np.float32)


def main():
    pack, extract = sys.argv[1], sys.argv[2]
    only = set(sys.argv[3:])
    hud = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "hud_txd.json")))
    work = os.path.join(pack, "work_hud")
    os.makedirs(work, exist_ok=True)
    files, ids = [], []
    for name, rid in sorted(hud.items()):
        if name in DONE or (only and name not in only):
            continue
        path = os.path.join(extract, rid + ".png")
        if not os.path.exists(path):
            continue
        src = np.asarray(Image.open(path).convert("RGBA"), np.float32) / 255
        ai = None
        if True:
            ap = os.path.join(pack, "work", rid + ".c.png")
            if os.path.exists(ap):
                ai = np.asarray(Image.open(ap).convert("RGBA"), np.float32) / 255
        out = os.path.join(work, rid + ".c.png")
        Image.fromarray(rebuild(src, name, ai, painted(name)), "RGBA").save(out)
        files.append(out)
        ids.append(rid)
        print(name, "painted" if painted(name) else "flat", flush=True)
    generate.texconv(files, "BC7_UNORM", os.path.join(pack, "textures"))
    index = os.path.join(pack, "materials.txt")
    lines = [l for l in open(index).read().splitlines() if l.split(" ")[0] not in ids]
    lines += ["%s ui 0 1.000 0.0000 0.150 -1 0" % rid for rid in ids]
    open(index, "w").write("\n".join(lines) + "\n")
    print("%d HUD pieces" % len(ids))


if __name__ == "__main__":
    main()
