"""The texture dictionaries of COD.PAK one by one: their offset and each texture's name, size and remaster id.
Finds which pictures belong together (the HUD's dictionary holds every HUD piece, shown or not).

    python -I txd_list.py <COD.PAK> [name substring...]   (only dictionaries holding a texture of such a name)
"""
import mmap
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract  # noqa: E402
import txd  # noqa: E402


def main():
    pak = sys.argv[1]
    want = [w.lower() for w in sys.argv[2:]]
    with open(pak, "rb") as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        for hit in re.finditer(rb"\x16\x00\x00\x00(?s:....)" + re.escape(txd.RW_VERSION), m):
            start = hit.start()
            size, = struct.unpack_from("<I", m, start + 4)
            end = start + 12 + size
            if end > len(m):
                continue
            rows = []
            for cid, p, csize, ver in txd.chunks(m, start + 12, end):
                if cid != 0x15 or ver != struct.unpack("<I", txd.RW_VERSION)[0]:
                    continue
                t = txd.parse_native(m, p, csize)
                if not t:
                    continue
                fmt = extract.xbox_format(t)
                rid = "?"
                if fmt is not None and t.width >= 4 and t.height >= 4:
                    level0 = t.data[:t.width * t.height * (1 if fmt == 0x0B else 4)]
                    rid = "%016x" % extract.remaster_id(t.width, t.height, fmt, level0, t.palette if fmt == 0x0B else None)
                rows.append((t.name, t.width, t.height, rid))
            if not rows or (want and not any(w in n.lower() or w == r for n, _, _, r in rows for w in want)):
                continue
            print("txd at %08x: %d textures" % (start, len(rows)))
            for n, w, h, r in rows:
                print("  %-32s %4dx%-4d %s" % (n, w, h, r))


if __name__ == "__main__":
    main()
