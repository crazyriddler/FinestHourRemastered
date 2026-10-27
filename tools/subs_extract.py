"""Subtitles burned into a movie, found against the original: each frame of the subtitled movie compared with the
same frame of the game's own, the bright pixels the original lacks are the text. Runs of frames with the same text
become one subtitle: its first and last frame and a picture of it (the subtitled frame's crop), for reading.

    python -I tools/subs_extract.py <subtitled movies folder> <original movies folder> <out folder>

Writes <out>/<movie>.json ([{"start": s, "end": s, "box": [x0, y0, x1, y1], "image": file}]) and the crops.
"""
import json
import os
import subprocess
import sys

import numpy as np
from PIL import Image

FF = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "tools", "ffmpeg",
                  "ffmpeg-master-latest-win64-gpl", "bin", "ffmpeg.exe")
W, H, FPS = 640, 480, 30


def frames(path):
    p = subprocess.Popen([FF, "-v", "error", "-i", path, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                         stdout=subprocess.PIPE)
    size = W * H * 3
    while True:
        b = p.stdout.read(size)
        if len(b) < size:
            break
        yield np.frombuffer(b, np.uint8).reshape(H, W, 3)
    p.wait()


def text_mask(ru, en):
    # the subtitles' letters: white over the picture, with a dark edge
    lr = ru.astype(np.int16).mean(2)
    le = en.astype(np.int16).mean(2)
    m = (lr > 185) & (lr - le > 45)
    # single specks of re-encoding noise out: a pixel kept with a neighbour lit too
    n = np.zeros_like(m)
    n[1:-1, 1:-1] = m[1:-1, 1:-1] & (m[:-2, 1:-1] | m[2:, 1:-1] | m[1:-1, :-2] | m[1:-1, 2:])
    return n


def box(m):
    ys, xs = np.nonzero(m)
    return [int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1]


def main():
    sub_dir, orig_dir, out = sys.argv[1:4]
    os.makedirs(out, exist_ok=True)
    for name in sorted(os.listdir(sub_dir)):
        if not name.lower().endswith(".bik"):
            continue
        movie = name[:-4]
        subs, cur = [], None
        for i, (ru, en) in enumerate(zip(frames(os.path.join(sub_dir, name)), frames(os.path.join(orig_dir, name)))):
            m = text_mask(ru, en)
            on = m.sum() > 120
            if on:
                rows = m.any(1)
                if cur is not None:
                    # the same subtitle while its letters stay where they were
                    inter = (m & cur["mask"]).sum()
                    union = (m | cur["mask"]).sum()
                    if inter / max(union, 1) > 0.45:
                        cur["end"] = i
                        cur["mask"] |= m
                        if m.sum() > cur["best_n"]:
                            cur["best_n"], cur["best"] = int(m.sum()), ru.copy()
                        continue
                    subs.append(cur)
                cur = {"start": i, "end": i, "mask": m.copy(), "best_n": int(m.sum()), "best": ru.copy()}
            elif cur is not None and i - cur["end"] > 2:
                subs.append(cur)
                cur = None
        if cur is not None:
            subs.append(cur)
        doc = []
        for k, s in enumerate(subs):
            if s["end"] - s["start"] < 8:  # under a third of a second: a flash in the picture, not a subtitle
                continue
            x0, y0, x1, y1 = box(s["mask"])
            pad = 6
            crop = s["best"][max(0, y0 - pad):min(H, y1 + pad), max(0, x0 - pad):min(W, x1 + pad)]
            img = "%s_%03d.png" % (movie, len(doc))
            Image.fromarray(crop).save(os.path.join(out, img))
            doc.append({"start": round(s["start"] / FPS, 3), "end": round((s["end"] + 1) / FPS, 3),
                        "box": [x0, y0, x1, y1], "image": img})
        json.dump(doc, open(os.path.join(out, movie + ".json"), "w"), indent=1)
        print(movie, len(doc), "subtitles")


if __name__ == "__main__":
    main()
