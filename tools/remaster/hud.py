"""HD pieces of the HUD drawn again from their shapes, for the small textures an upscaler cannot sharpen: the
crosshair's tick (8 by 16, a white bar with a faint glow) at 16 times. Adds them to the pack and to materials.txt.

    python -I hud.py <pack folder>
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate  # noqa: E402

S = 16


def box(w, h, x0, y0, x1, y1, soft):
    """coverage of the rectangle [x0, x1) x [y0, y1) (in the original's pixels) at S times, edges soft over soft px"""
    ys, xs = np.mgrid[0:h * S, 0:w * S].astype(np.float32) + 0.5
    ys, xs = ys / S, xs / S
    dx = np.maximum(x0 - xs, xs - x1)
    dy = np.maximum(y0 - ys, ys - y1)
    d = np.maximum(dx, dy)  # signed distance outside, in original pixels
    return np.clip(0.5 - d * S / soft, 0, 1), d


def crosshair_tick():
    w, h = 8, 16
    bar, d = box(w, h, 1, 4, 7, 15, 1.5)
    glow = np.clip(1 - np.maximum(d, 0) / 1.2, 0, 1) * 0.12  # the original's faint halo, a pixel wide
    a = np.maximum(bar, glow)
    img = np.ones((h * S, w * S, 4), np.float32)
    img[..., 3] = a
    return (img * 255 + 0.5).astype(np.uint8)



# --- the compass: drawn at 8 times 512 and filtered down, in the originals' greys (the game tints them) -----------

FONTS = os.environ.get("FONTS_DIR") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                                                    "tools", "fonts")
DEJAVU = os.path.join(FONTS, "dejavu", "dejavu-fonts-ttf-2.37", "ttf")


def canvas(size):
    return Image.new("RGBA", (size, size), (0, 0, 0, 0))


def down(im, size):
    return np.asarray(im.resize((size, size), Image.LANCZOS))


def compass_bg():
    """the dial: a dark disc, a ring of ticks every 5 degrees with numbers every 30, the cardinal letters, a rose"""
    from PIL import ImageDraw, ImageFont
    big, out = 4096, 512
    im = canvas(big)
    d = ImageDraw.Draw(im)
    c = big / 2
    R = big * 0.345  # the original's dial
    d.ellipse((c - R, c - R, c + R, c + R), fill=(38, 40, 38, 205))
    d.ellipse((c - R, c - R, c + R, c + R), outline=(150, 150, 150, 210), width=int(big * 0.006))
    r1 = R * 0.93
    for deg in range(0, 360, 5):
        a = np.radians(deg)
        long_ = deg % 30 == 0
        r0 = r1 - (R * 0.11 if long_ else R * 0.06)
        w = int(big * (0.006 if long_ else 0.0035))
        d.line((c + r0 * np.sin(a), c - r0 * np.cos(a), c + r1 * np.sin(a), c - r1 * np.cos(a)),
               fill=(205, 205, 205, 235), width=w)
    num = ImageFont.truetype(os.path.join(DEJAVU, "DejaVuSansCondensed.ttf"), int(big * 0.03))
    for deg in range(30, 360, 30):
        if deg % 90 == 0:
            continue
        a = np.radians(deg)
        rr = R * 0.72
        d.text((c + rr * np.sin(a), c - rr * np.cos(a)), str(deg), font=num, fill=(200, 200, 200, 230), anchor="mm")
    letters = ImageFont.truetype(os.path.join(DEJAVU, "DejaVuSerif-Bold.ttf"), int(big * 0.075))
    for deg, ch in ((0, "N"), (90, "E"), (180, "S"), (270, "W")):
        a = np.radians(deg)
        rr = R * 1.0
        x, y = c + rr * np.sin(a), c - rr * np.cos(a)
        d.text((x, y), ch, font=letters, fill=(245, 245, 245, 255), anchor="mm", stroke_width=int(big * 0.008),
               stroke_fill=(30, 30, 30, 255))
    # the rose: 4 long points and 4 short, each half light and half shade
    def point(angle, length, width, light, shade):
        a = np.radians(angle)
        tip = (c + length * np.sin(a), c - length * np.cos(a))
        l = (c + width * np.sin(a - np.pi / 2), c - width * np.cos(a - np.pi / 2))
        r = (c + width * np.sin(a + np.pi / 2), c - width * np.cos(a + np.pi / 2))
        d.polygon([(c, c), l, tip], fill=light)
        d.polygon([(c, c), tip, r], fill=shade)
    for ang in range(45, 360, 90):
        point(ang, R * 0.42, R * 0.07, (190, 190, 190, 245), (120, 120, 120, 245))
    for ang in range(0, 360, 90):
        point(ang, R * 0.62, R * 0.09, (250, 250, 250, 255), (165, 165, 165, 255))
    d.ellipse((c - R * 0.035, c - R * 0.035, c + R * 0.035, c + R * 0.035), fill=(60, 60, 60, 255))
    return down(im, out)


def compass_glass():
    """the glass over the dial: a thin rim and two highlights, mostly clear"""
    from PIL import ImageDraw, ImageFilter
    big, out = 2048, 512
    im = canvas(big)
    d = ImageDraw.Draw(im)
    c, R = big / 2, big * 0.35  # over the dial
    d.ellipse((c - R, c - R, c + R, c + R), outline=(230, 230, 230, 150), width=int(big * 0.008))
    hl = canvas(big)
    hd = ImageDraw.Draw(hl)
    hd.ellipse((c - R * 0.8, c - R * 0.85, c + R * 0.2, c - R * 0.05), fill=(255, 255, 255, 46))
    hd.ellipse((c + R * 0.3, c + R * 0.35, c + R * 0.62, c + R * 0.62), fill=(255, 255, 255, 70))
    hl = hl.filter(ImageFilter.GaussianBlur(big * 0.03))
    mask = canvas(big)
    ImageDraw.Draw(mask).ellipse((c - R, c - R, c + R, c + R), fill=(255, 255, 255, 255))
    clipped = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    clipped.paste(hl, (0, 0), mask.getchannel("A"))
    im.alpha_composite(clipped)
    return down(im, out)


def star_points(c, r_out, r_in, rot=-90):
    pts = []
    for i in range(10):
        r = r_out if i % 2 == 0 else r_in
        a = np.radians(rot + i * 36)
        pts.append((c + r * np.cos(a), c + r * np.sin(a)))
    return pts


def compass_star():
    """the objective's medal: a gold ring with a five pointed star, lit from the top left"""
    from PIL import ImageDraw
    big, out = 2048, 256
    im = canvas(big)
    d = ImageDraw.Draw(im)
    c = big / 2
    R = big * 0.46
    gold, light, dark = (176, 166, 96, 255), (226, 216, 150, 255), (96, 88, 44, 255)
    d.ellipse((c - R, c - R, c + R, c + R), fill=dark)
    d.ellipse((c - R * 0.93, c - R * 0.95, c + R * 0.9, c + R * 0.88), fill=light)
    d.ellipse((c - R * 0.86, c - R * 0.86, c + R * 0.86, c + R * 0.86), fill=gold)
    d.ellipse((c - R * 0.7, c - R * 0.7, c + R * 0.7, c + R * 0.7), fill=dark)
    d.ellipse((c - R * 0.64, c - R * 0.64, c + R * 0.66, c + R * 0.66), fill=(126, 118, 62, 255))
    d.polygon(star_points(c + R * 0.02, R * 0.74, R * 0.3), fill=dark)
    d.polygon(star_points(c, R * 0.7, R * 0.28), fill=gold)
    # the star's facets: each point's left half lighter
    pts = star_points(c, R * 0.7, R * 0.28)
    for i in range(0, 10, 2):
        tip, inner = pts[i], pts[(i - 1) % 10]
        d.polygon([(c, c), inner, tip], fill=light)
    return down(im, out)


def compass_pulse():
    """the objective's glow: a soft bright ring"""
    big, out = 1024, 256
    ys, xs = np.mgrid[0:big, 0:big].astype(np.float32) + 0.5
    r = np.sqrt((xs - big / 2) ** 2 + (ys - big / 2) ** 2) / (big / 2)
    a = np.exp(-((r - 0.78) / 0.11) ** 2) * 0.7
    img = np.zeros((big, big, 4), np.float32)
    img[..., 0], img[..., 1], img[..., 2] = 1.0, 0.965, 0.643
    img[..., 3] = a
    return down(Image.fromarray((img * 255).astype(np.uint8), "RGBA"), out)


def compass_star_arrow():
    """the objective's direction at the dial's edge: a gold triangle, drawn added (black is nothing)"""
    from PIL import ImageDraw
    big, out = 1024, 128
    im = Image.new("RGBA", (big, big), (0, 0, 0, 255))
    d = ImageDraw.Draw(im)
    tip, base_y, half = (big / 2, big * 0.18), big * 0.84, big * 0.36
    d.polygon([tip, (big / 2 + half, base_y), (big / 2 - half, base_y)], fill=(96, 88, 44, 255))
    d.polygon([(big / 2, big * 0.27), (big / 2 + half * 0.78, base_y - big * 0.05),
               (big / 2 - half * 0.78, base_y - big * 0.05)], fill=(170, 160, 100, 255))
    d.polygon([(big / 2, big * 0.27), (big / 2, base_y - big * 0.05), (big / 2 - half * 0.78, base_y - big * 0.05)],
              fill=(205, 196, 132, 255))
    return down(im, out)


def compass_arrow():
    """the heading's arrow: a red-brown arrow pointing up"""
    from PIL import ImageDraw
    w, h, k = 16, 32, 64
    im = Image.new("RGBA", (w * k, h * k), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    cx = w * k / 2
    d.polygon([(cx, 1.5 * k), (cx + 6 * k, 9 * k), (cx + 2.2 * k, 9 * k), (cx + 2.2 * k, 31 * k),
               (cx - 2.2 * k, 31 * k), (cx - 2.2 * k, 9 * k), (cx - 6 * k, 9 * k)], fill=(68, 36, 26, 255))
    d.polygon([(cx, 3 * k), (cx + 4.6 * k, 8.2 * k), (cx + 1.4 * k, 8.2 * k), (cx + 1.4 * k, 30 * k),
               (cx - 1.4 * k, 30 * k), (cx - 1.4 * k, 8.2 * k), (cx - 4.6 * k, 8.2 * k)], fill=(132, 76, 56, 255))
    return np.asarray(im.resize((w * 8, h * 8), Image.LANCZOS))


def cursor():
    """the menus' mouse pointer: a classic arrow in the compass medal's brass, a dark outline and a soft shadow"""
    from PIL import ImageDraw, ImageFilter
    big, out = 1024, 64
    pts = [(0.08, 0.04), (0.08, 0.80), (0.27, 0.62), (0.40, 0.92), (0.53, 0.86), (0.40, 0.57), (0.66, 0.57)]
    P = [(x * big, y * big) for x, y in pts]
    shadow = Image.new("L", (big, big), 0)
    ImageDraw.Draw(shadow).polygon([(x + big * 0.03, y + big * 0.04) for x, y in P], fill=150)
    shadow = shadow.filter(ImageFilter.GaussianBlur(big * 0.025))
    im = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    im.putalpha(shadow)
    d = ImageDraw.Draw(im)
    d.polygon(P, fill=(44, 36, 20, 255))  # the outline: the shape, then the fill inset
    inner = Image.new("L", (big, big), 0)
    ImageDraw.Draw(inner).polygon(P, fill=255)
    inner = inner.filter(ImageFilter.MinFilter(int(big * 0.045) | 1))
    # brass: light at the top left, darker towards the tail
    ys, xs = np.mgrid[0:big, 0:big].astype(np.float32) / big
    t = np.clip((xs + ys) / 1.2, 0, 1)[..., None]
    light, dark = np.array([236, 224, 160], np.float32), np.array([150, 138, 74], np.float32)
    col = light * (1 - t) + dark * t
    fill = Image.fromarray(np.dstack([col, np.full((big, big, 1), 255, np.float32)]).astype(np.uint8), "RGBA")
    im.paste(fill, (0, 0), inner)
    return down(im, out)


PIECES = {
    "a56bd892e4cb0adf": crosshair_tick,
    "dec48f377e5b4684": compass_bg,
    "3b85684838815a71": compass_glass,
    "6dbbaae5119e1354": compass_star,
    "c0c3891843e894c3": compass_pulse,
    "cb0a77345ca0e523": compass_star_arrow,
    "92c1dfb682b56d5d": compass_arrow,
}


def main():
    pack = sys.argv[1]
    work = os.path.join(pack, "work_hud")
    os.makedirs(work, exist_ok=True)
    files = []
    for rid, make in PIECES.items():
        path = os.path.join(work, rid + ".c.png")
        Image.fromarray(make(), "RGBA").save(path)
        files.append(path)
    generate.texconv(files, "BC7_UNORM", os.path.join(pack, "textures"))
    # the overlay's own pictures, plain RGBA under ui/
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import prompts
    os.makedirs(os.path.join(pack, "ui"), exist_ok=True)
    prompts.save_dds(os.path.join(pack, "ui", "cursor.dds"), Image.fromarray(cursor(), "RGBA"))
    # materials.txt: the pieces as plain ui textures
    index = os.path.join(pack, "materials.txt")
    lines = [l for l in open(index).read().splitlines() if l.split(" ")[0] not in PIECES]
    lines += ["%s ui 0 1.000 0.0000 0.150 -1 0" % rid for rid in PIECES]
    open(index, "w").write("\n".join(lines) + "\n")
    print("%d HUD pieces" % len(PIECES))


if __name__ == "__main__":
    main()
