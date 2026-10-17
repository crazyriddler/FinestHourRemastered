"""Grid of screenshots with their names: montage.py out.png a.png b.png ... [--cols N] [--w 480]"""
import argparse
import os

from PIL import Image, ImageDraw

ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("files", nargs="+")
ap.add_argument("--cols", type=int, default=3)
ap.add_argument("--w", type=int, default=480)
a = ap.parse_args()
imgs = []
for f in a.files:
    if not os.path.exists(f):
        continue
    im = Image.open(f).convert("RGB")
    h = int(im.height * a.w / im.width)
    imgs.append((os.path.basename(f), im.resize((a.w, h))))
if not imgs:
    raise SystemExit("no images")
cols = min(a.cols, len(imgs))
rows = (len(imgs) + cols - 1) // cols
ch = max(im.height for _, im in imgs) + 16
out = Image.new("RGB", (cols * a.w, rows * ch), (40, 40, 40))
d = ImageDraw.Draw(out)
for i, (name, im) in enumerate(imgs):
    x, y = (i % cols) * a.w, (i // cols) * ch
    out.paste(im, (x, y + 16))
    d.text((x + 4, y + 2), name, fill=(255, 255, 0))
out.save(a.out)
print(a.out)
