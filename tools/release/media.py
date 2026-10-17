"""The README's pictures from the trailer's takes (tools/shoot.py): the before/after pairs, the hero picture and the
GIFs, sized for a web page, under docs/media.

    python tools/release/media.py [--clips work/trailer/clips]

media.json beside the clips: {"hero": "<still>", "compare": ["<shot>", ...], "gifs": [["<clip>", start, seconds, "<name>"]]}
"""
import argparse
import json
import os
import subprocess
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MEDIA = os.path.join(ROOT, "docs", "media")


def save(src, dst, width):
    im = Image.open(src).convert("RGB")
    if im.width > width:
        im = im.resize((width, round(im.height * width / im.width)), Image.LANCZOS)
    if dst.endswith(".jpg"):
        im.save(dst, quality=90, optimize=True, progressive=True)
    else:
        im.save(dst, optimize=True)
    print("%s (%.0f KB)" % (os.path.relpath(dst, ROOT), os.path.getsize(dst) / 1024))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clips", default=os.path.join(ROOT, "work", "trailer", "clips"))
    a = ap.parse_args()
    spec = json.load(open(os.path.join(a.clips, "media.json")))
    os.makedirs(MEDIA, exist_ok=True)
    save(os.path.join(a.clips, spec["hero"] + ".png"), os.path.join(MEDIA, "hero.jpg"), 1920)
    for i, shot in enumerate(spec["compare"], 1):
        save(os.path.join(a.clips, shot + "_off.png"), os.path.join(MEDIA, "compare_%d_off.jpg" % i), 1280)
        save(os.path.join(a.clips, shot + ".png"), os.path.join(MEDIA, "compare_%d_on.jpg" % i), 1280)
    for clip, start, seconds, name in spec.get("gifs", []):
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "release", "trailer.py"), "--gif",
                        os.path.join(a.clips, clip + ".mp4"), str(start), str(seconds), os.path.join(MEDIA, name + ".gif"),
                        "--width", "512"], check=True)


if __name__ == "__main__":
    main()
