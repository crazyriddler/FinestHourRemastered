"""The English text behind a translation's lines (lang/<x>.txt, src/game/language.cpp), found again in the game's
files: "@<hash>" lines in the sound banks' English subtitles (SB_AGD/*/ENGLISH/*.agd: zero ended lines) and in the
levels' string assets (COD.PAK's chunks 0x719: the text past a 0x1C header), by the FNV-1a 64 of the Latin-1 bytes;
"$key" rows in COD.PAK's string tables (their English column). Written as a sheet, one line each: key, English, the
translation's text (tab separated, UTF-8, line breaks as \\n). Game text: work files only, never shipped.

    python -I tools/lang_sources.py <game folder> lang/pt.txt <out.tsv>
"""
import csv
import io
import os
import re
import struct
import sys


def fnv(b):
    h = 0xcbf29ce484222325
    for c in b:
        h = ((h ^ c) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def load(path):
    keys, lines = {}, {}
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\r\n")
        if not line or line.startswith("#") or "\t" not in line:
            continue
        k, t = line.split("\t", 1)
        if k.startswith("@"):
            lines[int(k[1:], 16)] = t
        else:
            keys[k] = t
    return keys, lines


def english(game):
    """every English text the game has: hash -> text, and the string tables' key -> English"""
    texts = {}
    for root, _, files in os.walk(os.path.join(game, "SB_AGD"), followlinks=True):
        if os.path.basename(root).upper() != "ENGLISH":
            continue
        for f in files:
            data = open(os.path.join(root, f), "rb").read()
            for m in re.finditer(rb"(?<=\x00)[\x20-\xff][\x09\x0a\x0d\x20-\xff]+(?=\x00)", data):
                texts.setdefault(fnv(m.group()), m.group().decode("latin-1"))
    pak = open(os.path.join(game, "COD.PAK"), "rb").read()
    tag = struct.pack("<I", 0x719)
    at = pak.find(tag)
    while at >= 0:
        size, = struct.unpack_from("<I", pak, at + 4)
        if 0x1C < size < 65536:
            body = pak[at + 12 + 0x1C:at + 12 + size]
            text = body.split(b"\0")[0]
            if text:
                texts.setdefault(fnv(text), text.decode("latin-1"))
        at = pak.find(tag, at + 4)
    tables = {}
    head = b"language,English,German,French,Italian,Spanish"
    at = pak.find(head)
    while at >= 0:
        end = pak.find(b"\0", at)
        for row in csv.reader(io.StringIO(pak[at:end].decode("latin-1"))):
            if len(row) >= 2 and row[0] and row[0] != "language":
                tables.setdefault(row[0], row[1])
        at = pak.find(head, at + 1)
    return texts, tables


def main():
    game, trans, out = sys.argv[1:4]
    keys, lines = load(trans)
    texts, tables = english(game)

    def esc(s):
        return s.replace("\r\n", "\\n").replace("\n", "\\n").replace("\t", " ")

    with open(out, "w", encoding="utf-8", newline="") as f:
        for k, t in keys.items():
            f.write("%s\t%s\t%s\n" % (k, esc(tables.get(k, "?")), t))
        for h, t in lines.items():
            f.write("@%016x\t%s\t%s\n" % (h, esc(texts.get(h, "?")), t))
    print("rows %d (%d with English), lines %d (%d with English)" % (
        len(keys), sum(1 for k in keys if k in tables), len(lines), sum(1 for h in lines if h in texts)))


if __name__ == "__main__":
    main()
