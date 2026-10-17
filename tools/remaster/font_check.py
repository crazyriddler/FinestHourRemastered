"""Checks the remastered font atlases against the originals, cell by cell (the RenderWare METRICS1 boxes of COD.PAK),
and draws a sheet of each: every character's original (4 times, each pixel a square) above its new cell, on grey,
the cell's edge in red.

Reported per atlas:
  edge   - ink (letter or outline) in the cell's outermost half pixel at its sides, or its top or bottom row: the game's
           quad ends there, what lies past it is cut
  lost   - a character the original draws and the new cell leaves empty
  base   - the new letter's foot more than a pixel (console) off the original's (letters without descenders)
  pair   - a family's plain and outlined atlases with their letters in different places (the game draws one over the
           other)

    python -I font_check.py <COD.PAK> <extract folder> <pack folder> <out folder>
"""
import json
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fonts  # noqa: E402

S = fonts.SCALE
PAIRS = [("fbld24", "fblds24")]
DESCENDERS = set("gjpqyQJ,;()[]{}|@$_")


def lab(ch):
    """a character as the console prints it"""
    return ch if ch.isascii() and ch.isprintable() else "U+%04X" % ord(ch)


def white(rgba):
    """the letter's own light (not its outline): alpha times brightness"""
    return rgba[..., 3] / 255.0 * rgba[..., :3].mean(axis=2) / 255.0


def check(name, orig, new, boxes):
    light, _ = fonts.masks(orig.astype(np.float32))
    wn = white(new)
    an = new[..., 3] / 255.0
    out = {"edge": [], "lost": [], "base": []}
    for ch, x0, y0, x1, y1 in boxes:
        if x1 <= x0 or y1 <= y0:
            continue
        o = light[y0:y1 + 1, x0:x1 + 1]
        if o.max() < 0.5:
            continue
        a = an[y0 * S:(y1 + 1) * S, x0 * S:(x1 + 1) * S]
        w = wn[y0 * S:(y1 + 1) * S, x0 * S:(x1 + 1) * S]
        if w.max() < 0.4:
            out["lost"].append(lab(ch))
            continue
        side = max(a[:, :S // 2].max(), a[:, -(S // 2):].max())
        tb = max(a[0].max(), a[-1].max())
        if side > 0.08 or tb > 0.08:
            out["edge"].append("%s(%s%.2f)" % (lab(ch), "side " if side > 0.08 else "top/bottom ", max(side, tb)))
        if ch not in DESCENDERS and ch.isalnum():
            ro = np.where(o.max(axis=1) > 0.5)[0]
            rn = np.where(w.max(axis=1) > 0.4)[0]
            if len(ro) and len(rn) and abs((rn[-1] + 1) / S - (ro[-1] + 1)) > 1.01:
                out["base"].append("%s(%+.1f)" % (lab(ch), (rn[-1] + 1) / S - (ro[-1] + 1)))
    return out


def pair(a_new, b_new, boxes):
    wa, wb = white(a_new) > 0.5, white(b_new) > 0.5
    bad = []
    for ch, x0, y0, x1, y1 in boxes:
        if x1 <= x0 or y1 <= y0:
            continue
        sa = wa[y0 * S:(y1 + 1) * S, x0 * S:(x1 + 1) * S]
        sb = wb[y0 * S:(y1 + 1) * S, x0 * S:(x1 + 1) * S]
        if not sa.any() or not sb.any():
            continue
        ca, cb = np.where(sa.any(0))[0], np.where(sb.any(0))[0]
        ra, rb = np.where(sa.any(1))[0], np.where(sb.any(1))[0]
        d = max(abs(ca[0] - cb[0]), abs(ca[-1] - cb[-1]), abs(ra[0] - rb[0]), abs(ra[-1] - rb[-1]))
        if d > 2:
            bad.append("%s(%d)" % (lab(ch), d))
    return bad


def sheet(name, orig, new, boxes, path):
    cells = [b for b in boxes if b[3] > b[1] and b[4] > b[2]]
    cw = max(b[3] - b[1] + 1 for b in cells) * S + 8
    chh = max(b[4] - b[2] + 1 for b in cells) * S
    cols = 16
    rows = (len(cells) + cols - 1) // cols
    img = Image.new("RGB", (cols * cw, rows * (2 * chh + 24)), (40, 40, 40))
    d = ImageDraw.Draw(img)
    o_img = Image.fromarray(orig.astype(np.uint8), "RGBA")
    n_img = Image.fromarray(new.astype(np.uint8), "RGBA")
    for i, (ch, x0, y0, x1, y1) in enumerate(cells):
        cx, cy = (i % cols) * cw + 4, (i // cols) * (2 * chh + 24) + 2
        w, h = (x1 - x0 + 1) * S, (y1 - y0 + 1) * S
        for k, t in enumerate((o_img.crop((x0, y0, x1 + 1, y1 + 1)).resize((w, h), Image.NEAREST),
                               n_img.crop((x0 * S, y0 * S, (x1 + 1) * S, (y1 + 1) * S)))):
            bg = Image.new("RGBA", (w, h), (120, 130, 150, 255))
            bg.alpha_composite(t)
            img.paste(bg.convert("RGB"), (cx, cy + k * chh))
            d.rectangle((cx - 1, cy + k * chh - 1, cx + w, cy + k * chh + h), outline=(220, 40, 40))
        label = ch if ch.isprintable() else "?"
        d.text((cx, cy + 2 * chh + 4), "%s %d" % (label, ord(ch)), fill=(255, 255, 0))
    img.save(path)


def main():
    pak, extract, pack, out = sys.argv[1:5]
    os.makedirs(out, exist_ok=True)
    metrics = fonts.read_metrics(pak)
    index = json.load(open(os.path.join(extract, "index.json")))
    atlases = {}
    for tid, info in index.items():
        name = os.path.splitext(info["names"][0])[0]
        if name not in fonts.FACES or name not in metrics:
            continue
        orig = np.asarray(Image.open(os.path.join(extract, tid + ".png")).convert("RGBA")).astype(np.float32)
        new = np.asarray(Image.open(os.path.join(pack, "work_fonts", tid + ".c.png")).convert("RGBA")).astype(np.float32)
        atlases.setdefault(name, []).append(new)
        r = check(name, orig, new, metrics[name])
        print("%s %s: %s" % (name, tid, "; ".join("%s %d %s" % (k, len(v), " ".join(v[:40])) for k, v in r.items() if v) or "clean"))
        sheet(name, orig, new, metrics[name], os.path.join(out, "%s_%s.png" % (name, tid)))
    for a, b in PAIRS:
        if a in atlases and b in atlases:
            bad = pair(atlases[a][0], atlases[b][0], metrics[a])
            print("pair %s/%s: %s" % (a, b, " ".join(bad) if bad else "the same"))


if __name__ == "__main__":
    main()
