"""High resolution fonts: the game's bitmap font atlases drawn again 4 times larger from open licence typefaces of the
same kind, each character in its own box of the atlas (RenderWare METRICS1 files in COD.PAK give the boxes), sized to
the original's cap height and baseline and placed over the original's ink. A dark outline or shadow the original has
is measured and drawn again. Characters a typeface lacks keep the original, upscaled.

    python -I fonts.py <COD.PAK> <extract folder> <pack folder>

Typefaces (../tools/fonts beside the repo, or FONTS_DIR): Jost (OFL) for the Futura Bold of fbld24/fblds24, Courier
Prime Bold (OFL) for the typewriter of lta32, DejaVu Sans (Bitstream Vera licence) for the small Tahoma-like t12.
"""
import json
import mmap
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate  # noqa: E402

FONTS = os.environ.get("FONTS_DIR") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                                                    "tools", "fonts")
SCALE = 4
# console pixels kept free around each glyph inside its box, per atlas family (the outlined ones' outline)
MARGIN = {"fbld24": 2.5, "fblds24": 2.5, "t12": 1.5, "lta32": 0.5}
# the outline's width (console pixels) a family's glyphs leave free at their sides, plain and outlined atlases alike
OUTLINE = {"fbld24": 2, "fblds24": 2, "t12": 1}

# atlas name: typeface file, variable font weight (None: a static face)
FACES = {
    "fbld24": ("Jost.ttf", 700),
    "fblds24": ("Jost.ttf", 700),
    "lta32": ("CourierPrime-Bold.ttf", None),
    "t12": (os.path.join("dejavu", "dejavu-fonts-ttf-2.37", "ttf", "DejaVuSansCondensed.ttf"), None),
}


def read_metrics(pak):
    """name (without extension) -> list of (char, x0, y0, x1, y1), boxes inclusive"""
    out = {}
    with open(pak, "rb") as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        i = m.find(b"METRICS1")
        while i >= 0:
            raw = m[i:m.find(b"\0", i)].split(b"\r\n")
            name = os.path.splitext(raw[1].split()[0].decode("latin-1"))[0]
            boxes = []
            for line in raw[3:]:
                parts = line.split(b"#")[0].split()
                if len(parts) >= 5:
                    code, x0, y0, x1, y1 = map(int, parts[:5])
                    boxes.append((bytes([code]).decode("cp1252", errors="replace"), x0, y0, x1, y1))
            out[name] = boxes
            i = m.find(b"METRICS1", i + 1)
    return out


def masks(rgba):
    """the glyphs' light ink and the dark outline or shadow, as alpha 0..1"""
    a = rgba[..., 3] / 255.0
    lum = rgba[..., :3].mean(axis=2) / 255.0
    return a * np.clip((lum - 0.3) / 0.4, 0, 1), a * np.clip((0.7 - lum) / 0.4, 0, 1)


def shift(m, dx, dy):
    out = np.zeros_like(m)
    h, w = m.shape
    xs, xd = (0, dx) if dx >= 0 else (-dx, 0)
    ys, yd = (0, dy) if dy >= 0 else (-dy, 0)
    out[yd:h - ys if ys else h, xd:w - xs if xs else w] = m[ys:h - yd if yd else h, xs:w - xd if xd else w]
    return out


def dilate(m, r):
    out = m.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            if dx * dx + dy * dy <= r * r + r:
                out = np.maximum(out, shift(m, dx, dy))
    return out


def shadow_style(light, dark):
    """offset and growth of the dark ink against the light one, or None"""
    if dark.sum() < light.sum() * 0.05:
        return None
    best, style = -1.0, None
    for r in (0, 1, 2):
        grown = dilate(light, r)
        for dy in range(-2, 3):
            for dx in range(-2, 3):
                s = np.clip(shift(grown, dx, dy) - light, 0, 1)
                score = (np.minimum(s, dark).sum() * 2) / (s.sum() + dark.sum() + 1e-6)
                if score > best:
                    best, style = score, (dx, dy, r, float(dark.max()))
    return style


def load_face(name, size):
    path, weight = FACES[name]
    face = ImageFont.truetype(os.path.join(FONTS, path), size)
    if weight is not None:
        face.set_variation_by_axes([weight])
    return face


def draw_atlas(name, src, boxes):
    """the atlas at SCALE times its size: RGBA uint8"""
    light, dark = masks(src)
    h, w = light.shape
    style = shadow_style(light, dark)
    # the original's cap height and baseline, from H
    ref = next(b for b in boxes if b[0] == "H")
    _, x0, y0, x1, y1 = ref
    rows = np.where(light[y0:y1 + 1, x0:x1 + 1].max(axis=1) > 0.5)[0]
    cap, base = rows[-1] - rows[0] + 1, rows[-1] + 1  # rows, baseline below the last
    # the size whose H is as tall, at SCALE
    size = int(cap * SCALE * 1.4)
    for _ in range(30):
        face = load_face(name, size)
        hb = face.getbbox("H", anchor="ls")
        hh = hb[3] - hb[1]
        if abs(hh - cap * SCALE) <= 1:
            break
        size = max(4, int(round(size * cap * SCALE / max(hh, 1))))
    face = load_face(name, size)
    # one size for the whole atlas: as large as the cap height asks, less if that leaves glyphs too wide for their
    # boxes (all letters of a text the same size; the few still too wide narrow a little)
    grow = int(MARGIN.get(name, 0.5) * SCALE)
    ratios = []
    for ch, bx0, by0, bx1, by1 in boxes:
        if bx1 <= bx0 or light[by0:by1 + 1, bx0:bx1 + 1].max() < 0.5 or not ch.isprintable() or ch == "�":
            continue
        gb = face.getbbox(ch, anchor="ls")
        if gb[2] > gb[0]:
            ratios.append(((bx1 - bx0 + 1) * SCALE - SCALE) / (gb[2] - gb[0]))
    fit = min(1.0, float(np.percentile(ratios, 8)) / 0.87) if ratios else 1.0  # up to 13% narrower is not seen
    if fit < 1:
        size = max(4, int(size * fit))
        face = load_face(name, size)
    W, H = w * SCALE, h * SCALE
    ink = np.zeros((H, W), np.float32)
    up = np.asarray(Image.fromarray((light * 255).astype(np.uint8)).resize((W, H), Image.LANCZOS)) / 255.0
    # the face's usual room beside a letter in the original (console pixels each side): the median over its letters
    sides = []
    for ch, bx0, by0, bx1, by1 in boxes:
        if not ch.isalnum() or bx1 <= bx0 or by1 <= by0:
            continue
        cl = np.where(light[by0:by1 + 1, bx0:bx1 + 1].max(axis=0) > 0.3)[0]
        if len(cl):
            sides.append(((bx1 - bx0 + 1) - (cl[-1] - cl[0] + 1)) / 2)
    bear = float(np.median(sides)) if sides else 1.0
    # kept free at a glyph's sides in its box: its outline's width and half a pixel; a family's plain atlas as its
    # outlined one (the game draws the outlined text and the plain over it: their letters the same width and place)
    r_out = OUTLINE.get(name, style[2] if style else 0)
    pad = int(r_out * SCALE) + SCALE // 2
    # the face's stroke (an l's stem across its middle, pixels at SCALE): a letter narrowed to fit its box gets back
    # what its strokes lost, thickened across (a j or an f squeezed came out thinner than the others)
    lm = np.asarray(face.getmask("l"), np.uint8).reshape(face.getmask("l").size[::-1])
    stem = float(np.count_nonzero(lm[lm.shape[0] // 2] > 127)) if lm.size else 0.0
    drawn = 0
    boxes_drawn = []
    for ch, bx0, by0, bx1, by1 in boxes:
        if by1 <= by0 or bx1 <= bx0:
            continue
        orig = light[by0:by1 + 1, bx0:bx1 + 1]
        if orig.max() < 0.5:
            continue  # a space, or nothing drawn there
        cols = np.where(orig.max(axis=0) > 0.3)[0]
        ocx = (bx0 + (cols[0] + cols[-1] + 1) / 2) * SCALE
        bw, bh = (bx1 - bx0 + 1) * SCALE, (by1 - by0 + 1) * SCALE
        has = face.getmask(ch).getbbox() is not None and ch.isprintable() and ch != "�"
        if not has:
            ink[by0 * SCALE:(by1 + 1) * SCALE, bx0 * SCALE:(bx1 + 1) * SCALE] = up[by0 * SCALE:(by1 + 1) * SCALE,
                                                                                     bx0 * SCALE:(bx1 + 1) * SCALE]
            continue
        gb = face.getbbox(ch, anchor="ls")
        gw, gh = gb[2] - gb[0], gb[3] - gb[1]
        tile = Image.new("L", (gw + 8, gh + 8), 0)
        ImageDraw.Draw(tile).text((4 - gb[0], 4 - gb[1]), ch, font=face, fill=255, anchor="ls")
        tile = tile.crop((4, 4, 4 + gw, 4 + gh))
        # to its ink at the sides (the face's box holds its side bearings: the letters came out narrower than the
        # original's, with gaps between)
        ib = tile.getbbox()
        if ib:
            tile = tile.crop((ib[0], 0, ib[2], gh))
            gw = ib[2] - ib[0]
        natural = (gw, gh)  # (the typeface's own size: FONT_SQUEEZE reports what is changed)
        # on the baseline where the original's sits on it (the typeface's J hangs below it, the game's does not)
        orows = np.where(orig.max(axis=1) > 0.3)[0]
        if len(orows) and orows[-1] + 1 - base <= 0.5 and gb[3] > 0.12 * cap * SCALE and gh - gb[3] > SCALE:
            gh -= gb[3]
            tile = tile.resize((gw, gh), Image.LANCZOS)
        # inside its own box with room for the outline: a glyph or outline past its box shows at the edge of its
        # neighbours' (the game's quads sample their whole box)
        # (its outline's width and half a pixel kept free at the sides too: the original's letters touch their boxes'
        # sides, their outlines came out cut at the start of a d, an o)
        # (a box too narrow for that keeps the original's own ink width: the letter before its outline, a subtitle's t
        # five pixels wide came out half as wide)
        oink = (cols[-1] - cols[0] + 1) * SCALE
        room_w, room_h = bw - 2 * pad, bh - 2 * grow
        narrow = room_w < 0.6 * oink
        if narrow:
            room_w = min(oink, bw - SCALE)
        if gh > room_h:  # a tall glyph shrinks: only in height when that is a little (its strokes as thick as the
            k = room_h / gh  # other letters': a j shrunk whole came out thin), whole when it is much
            gw, gh = (gw if k >= 0.7 else max(1, int(gw * k))), max(1, int(gh * k))
            tile = tile.resize((gw, gh), Image.LANCZOS)
        if gw > room_w:  # one of the few still too wide narrows a little
            tile = tile.resize((max(1, room_w), gh), Image.LANCZOS)
            gw = room_w
        # as wide as the original's ink where that is near ours: the original's spacing kept (a narrower letter left
        # gaps beside it, "es colta"); its height and baseline as they are
        ink_w = (cols[-1] - cols[0] + 1) * SCALE
        # (a letter whose ink fills its box as wide as the box leaves room for)
        ink_w = min(ink_w, room_w)
        # a letter or a digit: the face's usual room on either side of it, in the middle of its box (the game advances
        # by the box; the original's own boxes are uneven, a c with a third of its box empty on the right, "Opc iones",
        # an E, "PE RFILES"); as wide as that leaves where that is near ours
        even = ch.isalnum()
        if even:  # (else its own width, the typeface's: the original's ink was the odd one)
            target = room_w if narrow else min(bw - 2 * max(bear * SCALE, pad), room_w)
            ink_w = target if 0.8 * gw < target < 1.25 * gw else gw
        if 0.75 * gw < ink_w < 1.3 * gw:
            tile = tile.resize((int(ink_w), gh), Image.LANCZOS)
            gw = int(ink_w)
        sx = gw / natural[0]
        # (narrowed again by what it then grows: the strokes stem * (gw - lost) / natural + lost, as thick as the face's)
        lost = int(round(stem * (1 - sx) / max(1 - stem / natural[0], 0.2)))
        if sx < 0.95 and lost >= 1 and gw - lost > 2:
            tile = tile.resize((gw - lost, gh), Image.LANCZOS)
            t = np.asarray(tile, np.float32)
            wide = np.zeros((gh, gw), np.float32)
            for dx in range(lost + 1):
                wide[:, dx:dx + t.shape[1]] = np.maximum(wide[:, dx:dx + t.shape[1]], t)
            tile = Image.fromarray(wide.astype(np.uint8))
        # on the original's baseline
        top = by0 * SCALE + base * SCALE + gb[1]
        if even:
            ocx = (bx0 + (bx1 + 1 - bx0) / 2) * SCALE
        left = int(round(ocx - gw / 2))
        side = max(0, min(pad, (bw - gw) // 2)) if narrow else pad
        left = min(max(left, bx0 * SCALE + side), (bx1 + 1) * SCALE - side - gw)
        top = min(max(top, by0 * SCALE + grow), (by1 + 1) * SCALE - grow - gh)
        if os.environ.get("FONT_SQUEEZE"):
            print("squeeze %s U+%04X %.2f %.2f" % (name, ord(ch), gw / natural[0], gh / natural[1]))
        t = np.asarray(tile) / 255.0
        region = ink[top:top + gh, left:left + gw]
        region[...] = np.maximum(region, t[:region.shape[0], :region.shape[1]])
        boxes_drawn.append((bx0 * SCALE, by0 * SCALE, (bx1 + 1) * SCALE, (by1 + 1) * SCALE))
        drawn += 1
    out = np.zeros((H, W, 4), np.float32)
    if style:
        # each box's outline from its own glyph alone
        dx, dy, r, strength = style
        for x0, y0, x1, y1 in boxes_drawn:
            sub = ink[y0:y1, x0:x1]
            sh = np.clip(shift(dilate(sub, r * SCALE) if r else sub, dx * SCALE, dy * SCALE), 0, 1) * strength
            # fading out over the box's last half pixel at its sides (the glyphs leave their outline room: a safety,
            # never a straight cut where a box ends)
            fade = max(2, SCALE // 2)
            cols_ = np.arange(x1 - x0, dtype=np.float32)
            taper = np.clip(np.minimum(cols_ + 0.5, (x1 - x0) - cols_ - 0.5) / fade, 0, 1)
            sh = sh * taper[None, :]
            out[y0:y1, x0:x1, 3] = np.maximum(out[y0:y1, x0:x1, 3], sh)
    a = ink + out[..., 3] * (1 - ink)
    out[..., :3] = (ink / np.maximum(a, 1e-6))[..., None]
    out[..., 3] = a
    print("%s: %d characters at %d px, %s" % (name, drawn, size, "shadow %s" % (style,) if style else "plain"))
    return (np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8)


def main():
    pak, extract, pack = sys.argv[1:4]
    metrics = read_metrics(pak)
    index = json.load(open(os.path.join(extract, "index.json")))
    work = os.path.join(pack, "work_fonts")
    os.makedirs(work, exist_ok=True)
    files = []
    for tid, info in index.items():
        name = os.path.splitext(info["names"][0])[0]
        if name not in FACES or name not in metrics:
            continue
        src = np.asarray(Image.open(os.path.join(extract, tid + ".png")).convert("RGBA")).astype(np.float32)
        img = draw_atlas(name, src, metrics[name])
        path = os.path.join(work, tid + ".c.png")
        Image.fromarray(img, "RGBA").save(path)
        files.append(path)
    generate.texconv(files, "BC7_UNORM", os.path.join(pack, "textures"))


if __name__ == "__main__":
    main()
