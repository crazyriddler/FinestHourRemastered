"""The lighting bake's captured geometry (lvNN.geo, d3d12.cpp CaptureGeometry): reading it, its numbers, a picture.

    python tools/bake/geo.py <lvNN.geo> [--top out.png] [--size 2048]
"""
import argparse
import struct
import sys

import numpy as np

HEAD = struct.Struct("<4sBBHQQII")


def read(path):
    """the draws: dicts of kind, flags, tex, pos (n x 3), tris (m x 3)"""
    data = open(path, "rb").read()
    draws, at = [], 0
    while at + HEAD.size <= len(data):
        magic, kind, flags, _, tex, ident, nv, nt = HEAD.unpack_from(data, at)
        if magic != b"GEO2":
            raise ValueError("bad record at %d" % at)
        at += HEAD.size
        if at + nv * 12 + nt * 12 > len(data):
            break  # (the last record cut short: the game closed while writing it)
        pos = np.frombuffer(data, np.float32, nv * 3, at).reshape(nv, 3)
        at += nv * 12
        tris = np.frombuffer(data, np.uint32, nt * 3, at).reshape(nt, 3)
        at += nt * 12
        draws.append({"kind": kind, "flags": flags, "tex": tex, "id": ident, "pos": pos, "tris": tris})
    return draws


def area(d):
    p = d["pos"][d["tris"]]
    return 0.5 * np.linalg.norm(np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0]), axis=1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("geo")
    ap.add_argument("--top")
    ap.add_argument("--size", type=int, default=2048)
    args = ap.parse_args()
    draws = read(args.geo)
    tris = sum(len(d["tris"]) for d in draws)
    print("%d draws, %d triangles" % (len(draws), tris))
    for kind in (0, 1):
        for flags in range(4):
            sel = [d for d in draws if d["kind"] == kind and d["flags"] == flags]
            if sel:
                a = sum(area(d).sum() for d in sel) / 1e4
                print("  kind %d flags %d: %5d draws %8d triangles %10.0f m2" % (kind, flags, len(sel),
                                                                               sum(len(d["tris"]) for d in sel), a))
    allp = np.concatenate([d["pos"] for d in draws])
    print("bounds", allp.min(axis=0).round(), allp.max(axis=0).round())
    if args.top:
        from PIL import Image, ImageDraw
        lo, hi = allp.min(axis=0), allp.max(axis=0)
        s = args.size / max(hi[0] - lo[0], hi[2] - lo[2])
        img = Image.new("RGB", (args.size, args.size))
        dr = ImageDraw.Draw(img)
        colours = {(0, 2): (200, 200, 200), (0, 0): (200, 120, 60), (1, 0): (80, 160, 255), (1, 2): (80, 255, 160)}
        # by height: the higher the brighter
        for d in sorted(draws, key=lambda d: d["pos"][:, 1].mean()):
            c = colours.get((d["kind"], d["flags"] & 2), (255, 0, 255))
            for t in d["tris"]:
                q = d["pos"][t]
                dr.polygon([((x - lo[0]) * s, (z - lo[2]) * s) for x, _, z in q], outline=c)
        img.save(args.top)


if __name__ == "__main__":
    main()
