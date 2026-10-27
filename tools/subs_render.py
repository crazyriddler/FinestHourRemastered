"""A language's movie subtitles (lang/<x>_movies.txt) drawn into lang/<x>_movies.bin for src/game/movie_subs.cpp: each
subtitle a picture of the movie's own 640x480 pixels (white letters, a dark edge), centred across, its top where the
text file puts it; lines wider than the picture made smaller to fit.

    python -I tools/subs_render.py lang/ru_movies.txt lang/ru_movies.bin [--preview <folder>]

The file: "FHMS", version 1, count; each subtitle: its movie's name (a byte of length, the letters), start and end in
seconds (floats), x, y, width, height (16 bit), then its pixels as runs: a 16 bit count of clear pixels, a 16 bit count
of drawn ones and that many (alpha, grey) pairs, until width x height. Little endian.
"""
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = os.path.join(HERE, "fonts", "Jost-wght.ttf")
W, H = 640, 480
SS = 4            # drawn at four times the size, then scaled down: smooth edges
SIZE = 24         # the letters' size in the movie's pixels (the subtitled movies' own)
LINE = 35         # from one line to the next
MAX_W = 620       # the widest a line may be
EDGE = 2          # the dark edge around the letters


def font(size):
    f = ImageFont.truetype(FONT, int(size * SS))
    try:
        f.set_variation_by_axes([500])  # medium weight
    except Exception:
        pass
    return f


def render(text):
    lines = text.split("\\n")
    size = SIZE
    f = font(size)
    widest = max(f.getlength(l) for l in lines) / SS
    if widest > MAX_W - 2 * EDGE:
        size = SIZE * (MAX_W - 2 * EDGE) / widest
        f = font(size)
    line = LINE * size / SIZE
    w = int(min(W, max(f.getlength(l) for l in lines) / SS + 2 * EDGE + 8))
    h = int(line * (len(lines) - 1) + size * 1.35 + 2 * EDGE + 6)
    big = Image.new("LA", (w * SS, h * SS), (0, 0))
    d = ImageDraw.Draw(big)
    for i, l in enumerate(lines):
        y = (EDGE + 3 + i * line) * SS
        d.text((w * SS / 2, y), l, font=f, anchor="ma", fill=(255, 255), stroke_width=EDGE * SS, stroke_fill=(0, 215))
    return big.resize((w, h), Image.LANCZOS)


def runs(img):
    px = list(img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata())
    out, i, n = bytearray(), 0, len(px)
    while i < n:
        skip = 0
        while i < n and px[i][1] == 0 and skip < 65535:
            skip, i = skip + 1, i + 1
        lit = []
        while i < n and px[i][1] != 0 and len(lit) < 65535:
            lit.append(px[i])
            i += 1
        out += struct.pack("<HH", skip, len(lit))
        for g, a in lit:
            out += bytes((a, g))
    return bytes(out)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    preview = sys.argv[sys.argv.index("--preview") + 1] if "--preview" in sys.argv else None
    subs = []
    for l in open(src, encoding="utf-8"):
        l = l.rstrip("\r\n")
        if not l or l.startswith("#"):
            continue
        movie, start, end, top, text = l.split("\t", 4)
        subs.append((movie, float(start), float(end), int(top), text))
    blob = bytearray(b"FHMS" + struct.pack("<II", 1, len(subs)))
    for k, (movie, start, end, top, text) in enumerate(subs):
        img = render(text)
        x = (W - img.width) // 2
        y = max(0, min(H - img.height, top - EDGE - 9))
        name = movie.encode("ascii")
        blob += bytes((len(name),)) + name + struct.pack("<ffHHHH", start, end, x, y, img.width, img.height) + runs(img)
        if preview:
            os.makedirs(preview, exist_ok=True)
            img.save(os.path.join(preview, "%s_%.2f.png" % (movie, start)))
    open(dst, "wb").write(blob)
    print(len(subs), "subtitles,", len(blob), "bytes ->", dst)


if __name__ == "__main__":
    main()
