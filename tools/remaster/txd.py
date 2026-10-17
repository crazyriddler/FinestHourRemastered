"""RenderWare 3.6 Xbox texture dictionaries (TXD) inside COD.PAK: find every texture native, read its header,
palette and pixel data as they sit on the disc (the bytes the game hands the GPU)."""
import mmap
import re
import struct

RW_VERSION = b"\xff\xff\x03\x18"  # 3.6.0.3 build stamp as the game's chunks carry it

# Xbox D3DFORMAT codes the textures use
FORMATS = {
    0x00: "L8", 0x01: "AL8", 0x02: "A1R5G5B5", 0x03: "X1R5G5B5", 0x04: "A4R4G4B4", 0x05: "R5G6B5",
    0x06: "A8R8G8B8", 0x07: "X8R8G8B8", 0x0B: "P8", 0x0C: "DXT1", 0x0E: "DXT3", 0x0F: "DXT5",
    0x10: "LIN_A1R5G5B5", 0x11: "LIN_R5G6B5", 0x12: "LIN_A8R8G8B8", 0x19: "A8", 0x1A: "A8L8",
}


class Texture:
    __slots__ = ("name", "mask", "width", "height", "depth", "levels", "raster_type", "dxt", "raster_format",
                 "palette", "data", "offset")


def chunks(m, start, end):
    p = start
    while p + 12 <= end:
        cid, size, ver = struct.unpack_from("<III", m, p)
        yield cid, p + 12, size, ver
        p += 12 + size


def parse_native(m, p, size):
    # texture native (0x15): struct (0x01) first
    cid, sp, ssize, _ = next(chunks(m, p, p + size))
    if cid != 1:
        return None
    platform, filt = struct.unpack_from("<II", m, sp)
    if platform != 5:
        return None
    t = Texture()
    t.name = m[sp + 8:sp + 40].split(b"\0")[0].decode("latin-1")
    t.mask = m[sp + 40:sp + 72].split(b"\0")[0].decode("latin-1")
    t.raster_format, has_alpha = struct.unpack_from("<II", m, sp + 72)
    t.width, t.height = struct.unpack_from("<HH", m, sp + 80)
    t.depth, t.levels, t.raster_type, t.dxt = struct.unpack_from("<BBBB", m, sp + 84)
    data_size, = struct.unpack_from("<I", m, sp + 88)
    q = sp + 92
    pal = 0
    if t.raster_format & 0x2000:
        pal = 256 * 4
    elif t.raster_format & 0x4000:
        pal = 32 * 4  # the console's smallest palette: 32 colours, 16 used
    t.palette = bytes(m[q:q + pal])
    q += pal
    t.data = bytes(m[q:q + data_size])
    t.offset = q
    return t


def textures(pak_path):
    with open(pak_path, "rb") as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        for hit in re.finditer(rb"\x16\x00\x00\x00(?s:....)" + re.escape(RW_VERSION), m):
            start = hit.start()
            size, = struct.unpack_from("<I", m, start + 4)
            end = start + 12 + size
            if end > len(m):
                continue
            for cid, p, csize, ver in chunks(m, start + 12, end):
                if cid == 0x15 and ver == struct.unpack("<I", RW_VERSION)[0]:
                    t = parse_native(m, p, csize)
                    if t:
                        yield t


if __name__ == "__main__":
    import collections
    import sys
    count = collections.Counter()
    sizes = collections.Counter()
    n = 0
    seen = set()
    for t in textures(sys.argv[1]):
        n += 1
        key = (t.name, len(t.data))
        seen.add(t.data[:64] + t.palette[:16])
        fmt = ("DXT%d" % {0xC: 1, 0xE: 3, 0xF: 5}.get(t.dxt, t.dxt)) if t.dxt else ("rf%04X" % t.raster_format)
        count[fmt] += 1
        sizes[(t.width, t.height)] += 1
        if n <= 5:
            print(t.name, t.width, t.height, t.depth, t.levels, hex(t.raster_format), t.dxt, len(t.data), len(t.palette))
    print(n, "textures,", len(seen), "distinct")
    print(count.most_common(20))
    print(sizes.most_common(12))
