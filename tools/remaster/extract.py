"""Every texture of the game from COD.PAK, decoded to PNG under its remaster id (the id the runtime gives it, see
sdk/runtime/gpu/remaster.cpp), with an index of names, sizes and formats.

    python -I extract.py <COD.PAK> <out folder>
"""
import json
import os
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import txd  # noqa: E402

MASK = (1 << 64) - 1
K0, K1 = 0x9E3779B97F4A7C15, 0xE7037ED1A0B428DB


def mix(a, b):
    r = (a & MASK) * (b & MASK)
    return (r & MASK) ^ (r >> 64)


def rt_hash(data, h=1469598103934665603):
    """gpu::Hash: 16 bytes a step through a 64 x 64 -> 128 bit multiply."""
    n = len(data)
    seed = (h ^ K0) & MASK
    full = n - n % 16
    if full:
        words = np.frombuffer(data[:full], dtype="<u8")
        for i in range(0, len(words), 2):
            seed = mix(int(words[i]) ^ K1, int(words[i + 1]) ^ seed)
    r = n - full
    if r:
        if r >= 8:
            x, = struct.unpack_from("<Q", data, full)
            y, = struct.unpack_from("<Q", data, n - 8)
        else:
            x = int.from_bytes(data[full:full + r] + b"\0" * (8 - r), "little")
            y = 0
        seed = mix(x ^ K1, y ^ seed ^ r)
    return mix(seed ^ K0, n ^ K1)


def palette_sig(pal):
    sig = []
    for i in range(16):
        b = pal[i * 4:i * 4 + 4] if i * 4 + 4 <= len(pal) else b"\0\0\0\0"
        sig.append((b[0] + b[1] + b[2] + b[3]) | (b[0] ^ b[1] ^ b[2] ^ b[3]) << 10 | max(b) << 18)
    return struct.pack("<16I", *sig)


def remaster_id(width, height, xbox_format, level0, palette):
    seed = (width << 40 ^ height << 16 ^ xbox_format ^ 0x5EED5EED) & MASK
    h = rt_hash(level0, seed)
    if palette is not None:
        h = rt_hash(palette_sig(palette), h)
    return h or 1


def unswizzle_index(w, h):
    """For each linear pixel, its position in the console's swizzled layout (bits of x and y interleaved, x first,
    while both have bits left, then the larger dimension's)."""
    xs = np.arange(w, dtype=np.uint64)
    ys = np.arange(h, dtype=np.uint64)
    ox = np.zeros(w, dtype=np.uint64)
    oy = np.zeros(h, dtype=np.uint64)
    bit = 0
    mx, my = 1, 1
    while mx < w or my < h:
        if mx < w:
            ox |= ((xs // mx) & 1) << np.uint64(bit)
            bit += 1
            mx <<= 1
        if my < h:
            oy |= ((ys // my) & 1) << np.uint64(bit)
            bit += 1
            my <<= 1
    return (oy[:, None] | ox[None, :]).astype(np.int64)


def xbox_format(t):
    """The D3DFORMAT the game gives the texture (the runtime's TextureDesc::format)."""
    rf = t.raster_format & 0x0F00
    if t.raster_format & (0x2000 | 0x4000):
        return 0x0B  # P8
    if rf == 0x0500:
        return 0x06  # A8R8G8B8
    if rf == 0x0600:
        return 0x07  # X8R8G8B8
    return None


def decode(t):
    """RGBA image, or None for a format not handled."""
    w, h = t.width, t.height
    fmt = xbox_format(t)
    idx = unswizzle_index(w, h)
    if fmt == 0x0B:
        n = w * h
        ind = np.frombuffer(t.data[:n], dtype=np.uint8)[idx]
        pal = np.frombuffer(t.palette.ljust(1024, b"\0"), dtype=np.uint8).reshape(-1, 4)[:, [2, 1, 0, 3]]  # BGRA
        rgba = pal[ind]
        if not (t.raster_format & 0x0F00) == 0x0500:  # 888 palettes: opaque
            rgba = rgba.copy()
            rgba[..., 3] = 255
        return Image.fromarray(rgba, "RGBA")
    if fmt in (0x06, 0x07):
        px = np.frombuffer(t.data[:w * h * 4], dtype=np.uint8).reshape(-1, 4)[idx]
        rgba = px[..., [2, 1, 0, 3]].copy()  # BGRA in memory
        if fmt == 0x07:
            rgba[..., 3] = 255
        return Image.fromarray(rgba, "RGBA")
    return None


def main():
    pak, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    index = {}
    for t in txd.textures(pak):
        fmt = xbox_format(t)
        if fmt is None or t.width < 4 or t.height < 4:
            continue
        level0 = t.data[:t.width * t.height * (1 if fmt == 0x0B else 4)]
        rid = "%016x" % remaster_id(t.width, t.height, fmt, level0, t.palette if fmt == 0x0B else None)
        if rid in index:
            index[rid]["names"] = sorted(set(index[rid]["names"]) | {t.name})
            continue
        img = decode(t)
        if img is None:
            continue
        img.save(os.path.join(out, rid + ".png"))
        index[rid] = {"names": [t.name], "mask": t.mask, "w": t.width, "h": t.height, "format": fmt,
                      "raster": t.raster_format, "alpha": bool(np.asarray(img)[..., 3].min() < 250)}
    with open(os.path.join(out, "index.json"), "w") as f:
        json.dump(index, f, indent=1, sort_keys=True)
    print(len(index), "textures")


if __name__ == "__main__":
    main()
