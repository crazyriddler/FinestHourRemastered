"""The squad's names over their heads in a language of the port's own (issue #47): each SoldierTrigger's name in the
levels' data in COD.PAK (its property 1, a NUL-ended string in a slot of its own, padded to 4 bytes) put in its place
as the game reads the file, in the slot's room. Writes lang/<x>_names.bin in the fonts' patches' format
(game/language.cpp LoadNames): "FHRFONT1", u64 COD.PAK's size, u32 count; each: u64 offset, u32 length, u32 kind 0,
the bytes (the name and NULs to the slot's end).

    python -I tools/lang_names.py <COD.PAK> lang/ru_names.txt lang/ru_names.bin
"""
import mmap
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lang_merge import game_bytes  # noqa: E402


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    pak, sheet, out = sys.argv[1:4]
    names = {}
    for line in open(sheet, encoding="utf-8"):
        line = line.rstrip("\r\n")
        if line and not line.startswith("#") and "\t" in line:
            en, tr = line.split("\t", 1)
            names[en] = tr
    f = open(pak, "rb")
    m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    entries, missing, wide = [], set(), set()
    for x in re.finditer(rb"SoldierTrigger\x00", m):
        j = m.find(b"\x01\x00\x00\x00", x.end(), x.end() + 64)
        if j < 4:
            continue
        size = struct.unpack_from("<I", m, j - 4)[0]
        if size < 9 or size > 64:
            continue
        slot = size - 8
        name = m[j + 4:j + 4 + slot].split(b"\0")[0].decode("latin-1")
        if name not in names:
            if name.startswith(("Pvt", "Sgt", "Cpl", "Lt.", "Cpt", "Cmsr")):
                missing.add(name)
            continue
        text, bad = game_bytes(names[name], True)
        if bad:
            sys.exit("%s: letters the game's fonts lack: %s" % (name, "".join(bad)))
        if len(text) >= slot:
            wide.add("%s -> %s (%d of %d)" % (name, names[name], len(text), slot - 1))
            continue
        entries.append((j + 4, text + b"\0" * (slot - len(text))))
    for n in sorted(missing):
        print("no translation:", n)
    for n in sorted(wide):
        print("too long, left in English:", n)
    with open(out, "wb") as o:
        o.write(b"FHRFONT1" + struct.pack("<QI", os.path.getsize(pak), len(entries)))
        for at, data in entries:
            o.write(struct.pack("<QII", at, len(data), 0) + data)
    print(len(entries), "names")


if __name__ == "__main__":
    main()
