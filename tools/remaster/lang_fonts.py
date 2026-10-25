"""Cyrillic for the game's fonts (Russian, src/game/language.cpp): the text in Windows-1251 bytes, its letters
(А-я at 0xC0-0xFF, Ё 0xA8, ё 0xB8) drawn in the atlases where the Latin-1 accented letters were (0xC0 on) and
the free rows below; the rest of each atlas as it is.

For each atlas the game draws text with (fbld24, fblds24, lta32, t12):
- the new boxes of those codes, in its METRICS1 file's lines (the same length: each number three wide);
- the letters drawn at four times the size by fonts.py's draw_atlas (Jost, DejaVu Sans; IBM Plex Mono for Courier
  Prime's missing Cyrillic), the whole atlas, as the remastered one under the new atlas's id (the pack);
- that drawn back at the atlas's own size in its 16 colours, for the pixels of those rows alone (the original
  atlas's game: Original preset, remastered textures off).
Written: <out>/ru_fonts.bin for the game (the bytes to put in COD.PAK's reads: the METRICS1 lines, and the atlases'
pixels in those rows, 0xFF where the game's own stay), the pack's new textures and their materials.

    python -I lang_fonts.py <COD.PAK> <pack folder> <out folder>

ru_fonts.bin: "FHRFONT1", u64 COD.PAK's size, u32 count; then each: u64 offset in COD.PAK, u32 length, u32 kind (0 the
bytes, 1 the bytes but 0xFF: the game's kept), the bytes.
"""
import json
import mmap
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract  # noqa: E402
import fonts  # noqa: E402
import generate  # noqa: E402
import txd  # noqa: E402

NAMES = ["fbld24", "fblds24", "lta32", "t12"]
S = fonts.SCALE
# the codes Russian draws its own way: Ё ё and А-я (№ written N: not in Jost)
CODES = [(0xA8, "Ё"), (0xB8, "ё")] + [(0xC0 + i, chr(0x410 + i)) for i in range(64)]


def metrics_lines(m):
    """name -> {code: [(offset, line bytes)]} of each METRICS1 file's lines, and its boxes"""
    out = {}
    i = m.find(b"METRICS1")
    while i >= 0:
        end = m.find(b"\0", i)
        raw = m[i:end]
        lines = raw.split(b"\r\n")
        name = os.path.splitext(lines[1].split()[0].decode("latin-1"))[0]
        at = i + len(lines[0]) + 2 + len(lines[1]) + 2 + len(lines[2]) + 2
        codes = {}
        for line in lines[3:]:
            parts = line.split(b"#")[0].split()
            if len(parts) >= 5:
                codes.setdefault(int(parts[0]), []).append((at, line))
            at += len(line) + 2
        out[name] = codes
        i = m.find(b"METRICS1", i + 1)
    return out


def layout(name, src, boxes, face_of):
    """the new boxes of CODES: in the place of the first of them (0xC0's) on, row after row to the atlas's end"""
    h, w = src.shape[:2]
    by = {ch: b for ch, *b in boxes}
    x0, y0, _, y1 = by["À"]
    rowh = y1 - y0 + 1
    step = sorted({b[1] for b in by.values() if b[1] > y0})[0] - y0 if any(b[1] > y0 for b in by.values()) else rowh
    light, _ = fonts.masks(src)
    # the room the original's letters leave beside them (their boxes less their ink), each side
    sides = []
    for ch, bx0, by0, bx1, by1 in boxes:
        if not ch.isalnum() or bx1 <= bx0:
            continue
        cl = np.where(light[by0:by1 + 1, bx0:bx1 + 1].max(axis=0) > 0.3)[0]
        if len(cl):
            sides.append(((bx1 - bx0 + 1) - (cl[-1] - cl[0] + 1)) / 2)
    side = float(np.median(sides)) if sides else 1.0
    # widths: the letters' ink at the atlas's size (each typeface sized to an H as tall as the original's), the room
    hx0, hy0, hx1, hy1 = by["H"]
    rows = np.where(light[hy0:hy1 + 1, hx0:hx1 + 1].max(axis=1) > 0.5)[0]
    cap = rows[-1] - rows[0] + 1
    # the typeface's letters against the original's: the boxes as wide as the original's for the same letters (its
    # face runs wider than the game's: the subtitles' Cyrillic came out a fifth wider spaced than their Latin)
    ratios = []
    for ch in "acenorsuvxzBEHKMOPTX":
        if ch in by:
            gb = face_of(ch, cap).getbbox(ch, anchor="ls")
            ratios.append((by[ch][2] - by[ch][0] + 1) / ((gb[2] - gb[0]) / S + 2 * side))
    k = float(np.median(ratios)) if ratios else 1.0
    out, x, y = [], x0, y0
    for code, ch in CODES:
        face = face_of(ch, cap)
        gb = face.getbbox(ch, anchor="ls")
        bw = int(round(k * ((gb[2] - gb[0]) / S + 2 * side)))
        bw = max(bw, 3)
        if x + bw > w:
            x, y = 0, y + step
        if y + rowh > h:
            sys.exit("%s: no room for %s" % (name, ch))
        out.append((code, ch, x, y, x + bw - 1, y + rowh - 1))
        x += bw
    return out, (x0, y0), step, rowh, cap


def faces_for(name):
    """a function: the typeface for a character at an H as tall as cap (pixels at the atlas's size), times S"""
    cache = {}

    def face_of(ch, cap):
        primary = cache.get(("p", cap))
        if primary is None:
            size = int(cap * S * 1.4)
            for _ in range(30):
                primary = fonts.load_face(name, size)
                hb = primary.getbbox("H", anchor="ls")
                if abs((hb[3] - hb[1]) - cap * S) <= 1:
                    break
                size = max(4, int(round(size * cap * S / max(hb[3] - hb[1], 1))))
            cache[("p", cap)] = primary
        if fonts.glyph_in(primary, ch) or name not in fonts.FALLBACK:
            return primary
        fb = cache.get(("f", cap))
        if fb is None:
            size = int(cap * S * 1.4)
            for _ in range(30):
                fb = fonts.load_face(name, size, fonts.FALLBACK)
                hb = fb.getbbox("H", anchor="ls")
                if abs((hb[3] - hb[1]) - cap * S) <= 1:
                    break
                size = max(4, int(round(size * cap * S / max(hb[3] - hb[1], 1))))
            cache[("f", cap)] = fb
        return fb
    return face_of


def quantize(rgba, palette):
    """each pixel's nearest of the palette's colours (premultiplied, alpha weighed): indices"""
    pal = np.frombuffer(palette, np.uint8).reshape(-1, 4)[:, [2, 1, 0, 3]].astype(np.float32)  # BGRA -> RGBA
    pp = np.concatenate([pal[:, :3] * pal[:, 3:4] / 255.0, pal[:, 3:4] * 1.5], axis=1)
    px = rgba.reshape(-1, 4).astype(np.float32)
    qp = np.concatenate([px[:, :3] * px[:, 3:4] / 255.0, px[:, 3:4] * 1.5], axis=1)
    used = 16
    d = ((qp[:, None, :] - pp[None, :used, :]) ** 2).sum(axis=2)
    return d.argmin(axis=1).astype(np.uint8).reshape(rgba.shape[:2])


def main():
    pak, pack, out = sys.argv[1:4]
    os.makedirs(out, exist_ok=True)
    work = os.path.join(pack, "work_fonts_ru")
    os.makedirs(work, exist_ok=True)
    f = open(pak, "rb")
    m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    pak_size = len(m)
    metrics = fonts.read_metrics(pak)
    lines = metrics_lines(m)
    texs = {}
    for t in txd.textures(pak):
        n = os.path.splitext(t.name)[0]
        if n in NAMES:
            texs.setdefault(n, []).append(t)
    entries = []
    mats = json.load(open(os.path.join(pack, "materials.json")))
    layouts = {}
    pngs = []
    for name in NAMES:
        boxes = metrics[name]
        face_of = faces_for(name)
        first = texs[name][0]
        src0 = np.asarray(extract.decode(first).convert("RGBA")).astype(np.float32)
        # (the plain and the outlined Futura the same boxes: the game draws one over the other)
        key = "fbld24" if name in ("fbld24", "fblds24") else name
        if key not in layouts:
            layouts[key] = layout(name, src0, boxes, face_of)
        new, (fx, fy), step, rowh, cap = layouts[key]
        remap = {code for code, _ in CODES}
        # the boxes kept, by code from the METRICS1 lines themselves (cp1252 decoding loses 0x81 and such)
        kept = []
        for code, ls in sorted(lines[name].items()):
            if code in remap:
                continue
            for _, line in ls:
                p = line.split(b"#")[0].split()
                ch = bytes([code]).decode("cp1252", errors="replace")
                kept.append((ch, int(p[1]), int(p[2]), int(p[3]), int(p[4])))
        all_boxes = kept + [(ch, x0, y0, x1, y1) for _, ch, x0, y0, x1, y1 in new]
        # the METRICS1 lines of the new boxes, as long as the old
        for code, ch, x0, y0, x1, y1 in new:
            for at, line in lines[name][code]:
                text = b"%3d %3d %3d %3d %3d # '" % (code, x0, y0, x1, y1) + bytes([code]) + b"'"
                if len(text) != len(line):
                    sys.exit("%s: line of %d is %d long, not %d" % (name, code, len(text), len(line)))
                entries.append((at, 0, text))
        for t in texs[name]:
            src = np.asarray(extract.decode(t).convert("RGBA")).astype(np.float32)
            h, w = src.shape[:2]
            free = np.zeros((h, w), bool)
            free[fy:fy + rowh, fx:] = True
            free[fy + rowh:, :] = True
            # rows past the last box of the original are free whatever they hold; the rows between already are
            work_src = src.copy()
            work_src[free] = 0
            # the new letters drawn roughly at the atlas's size: draw_atlas measures their ink in their boxes
            img = Image.fromarray(work_src.astype(np.uint8), "RGBA")
            dr = ImageDraw.Draw(img)
            light, _ = fonts.masks(src)
            hx0, hy0, hx1, hy1 = [b for b in boxes if b[0] == "H"][0][1:]
            rows = np.where(light[hy0:hy1 + 1, hx0:hx1 + 1].max(axis=1) > 0.5)[0]
            base = rows[-1] + 1
            for code, ch, x0, y0, x1, y1 in new:
                face = face_of(ch, cap)
                small = fonts.ImageFont.truetype(face.path, max(4, int(round(face.size / S))))
                gb = small.getbbox(ch, anchor="ls")
                cx = (x0 + x1 + 1) / 2 - (gb[0] + gb[2]) / 2
                dr.text((cx, y0 + base), ch, font=small, fill=(255, 255, 255, 255), anchor="ls")
            proxy = np.asarray(img).astype(np.float32)
            hi = fonts.draw_atlas(name, proxy, all_boxes)
            # back at the atlas's size: the mean of each 4 x 4 (premultiplied)
            hf = hi.astype(np.float32)
            pm = np.concatenate([hf[..., :3] * hf[..., 3:4] / 255.0, hf[..., 3:4]], axis=2)
            lo = pm.reshape(h, S, w, S, 4).mean(axis=(1, 3))
            a = lo[..., 3:4]
            lo_rgba = np.concatenate([np.where(a > 0, lo[..., :3] * 255.0 / np.maximum(a, 1e-6), 0), a], axis=2)
            lo_rgba = np.clip(lo_rgba, 0, 255)
            idx_new = quantize(lo_rgba, t.palette)
            n = w * h
            orig = np.frombuffer(t.data[:n], np.uint8)
            swz = extract.unswizzle_index(w, h)
            linear = orig[swz].copy()
            # (transparent: the colour most of the atlas is)
            bg = np.bincount(linear.ravel()).argmax()
            idx_new[lo_rgba[..., 3] < 8] = bg
            linear[free] = idx_new[free]
            raster = np.zeros(n, np.uint8)
            raster[swz.ravel()] = linear.ravel()
            masked = np.full(n, 0xFF, np.uint8)
            fl = free.ravel()
            masked[swz.ravel()[fl]] = linear.ravel()[fl]
            entries.append((t.offset, 1, masked.tobytes()))
            rid = "%016x" % extract.remaster_id(w, h, 0x0B, raster.tobytes(), t.palette)
            old = "%016x" % extract.remaster_id(w, h, 0x0B, t.data[:n], t.palette)
            print("%s @%d: %s -> %s, %d letters" % (name, t.offset, old, rid, len(new)))
            mats[rid] = dict(mats[old])
            path = os.path.join(work, rid + ".c.png")
            Image.fromarray(hi, "RGBA").save(path)
            pngs.append(path)
            # the sheet to look at: the atlas at its size as the game has it now, and the drawn one
            pal = np.frombuffer(t.palette.ljust(1024, b"\0"), np.uint8).reshape(-1, 4)[:, [2, 1, 0, 3]]
            Image.fromarray(pal[linear], "RGBA").save(os.path.join(work, rid + "_1x.png"))
    generate.texconv(pngs, "BC7_UNORM", os.path.join(pack, "textures"))
    json.dump(mats, open(os.path.join(pack, "materials.json"), "w"), indent=1, sort_keys=True)
    with open(os.path.join(out, "ru_fonts.bin"), "wb") as o:
        o.write(b"FHRFONT1" + struct.pack("<QI", pak_size, len(entries)))
        for at, kind, data in entries:
            o.write(struct.pack("<QII", at, len(data), kind) + data)
    print(len(entries), "patches")


if __name__ == "__main__":
    main()
