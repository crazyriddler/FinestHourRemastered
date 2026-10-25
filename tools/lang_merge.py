"""A translation's parts (work/<x>/part_N.tsv: key<TAB>text, made from src_N.tsv) into lang/<x>.txt, checked against
the English (src_N.tsv) and the game's room for it: the same keys; the button pictures and other tokens kept; every
character one the game's fonts have (Russian: language.cpp's GameChar); a subtitle within its 256 byte slot; a string
table's row within its length (the five languages' row). Problems are printed; --write writes the file anyway.

    python -I tools/lang_merge.py <game folder> work/ru lang/ru.txt Russian [--write]
"""
import csv
import glob
import io
import os
import re
import sys

TOKENS = re.compile(r"\$[A-Za-z0-9_]+\$|%[0-9.]*[a-z]|\{SaveGameSize\}|<Empty>|\\n|\$[A-Za-z]+(?:\([0-9]+\))?")
K1252 = {0x20AC: 0x80, 0x2026: 0x85, 0x2018: 0x91, 0x2019: 0x92, 0x201C: 0x93, 0x201D: 0x94, 0x2022: 0x95,
         0x2013: 0x96, 0x2014: 0x97, 0x2122: 0x99, 0x201E: 0x84, 0x201A: 0x82}


def game_bytes(text, russian):
    out, bad = bytearray(), []
    for ch in text:
        cp = ord(ch)
        if russian and 0x410 <= cp <= 0x44F:
            out.append(0xC0 + cp - 0x410)
        elif russian and cp == 0x401:
            out.append(0xA8)
        elif russian and cp == 0x451:
            out.append(0xB8)
        elif russian and 0xC0 <= cp <= 0xFF:
            bad.append(ch)
            out.append(0x3F)
        elif cp <= 0xFF:
            out.append(cp)
        elif cp in K1252:
            out.append(K1252[cp])
        else:
            bad.append(ch)
            out.append(0x3F)
    return bytes(out), bad


def rows_of(pak):
    """a string table's key -> its row's length"""
    data = open(pak, "rb").read()
    head = b"language,English,German,French,Italian,Spanish"
    out = {}
    at = data.find(head)
    while at >= 0:
        end = data.find(b"\0", at)
        text = data[at:end].decode("latin-1")
        p = text.find("\n")
        while 0 <= p < len(text) - 1:
            start, e, q = p + 1, p + 1, False
            while e < len(text):
                if text[e] == '"':
                    q = not q
                elif not q and text[e] in "\r\n":
                    break
                e += 1
            row = text[start:e]
            if "," in row:
                out.setdefault(row.split(",", 1)[0], len(row))
            p = text.find("\n", e)
        at = data.find(head, at + 1)
    return out


def field(t):
    if not any(c in t for c in ',"\r\n'):
        return t
    return '"' + t.replace('"', '""') + '"'


def main():
    game, folder, out, language = sys.argv[1:5]
    write = "--write" in sys.argv
    russian = language.lower() == "russian"
    rows = rows_of(os.path.join(game, "COD.PAK"))
    problems = 0
    lines = []
    for src in sorted(glob.glob(os.path.join(folder, "src_*.tsv"))):
        part = src.replace("src_", "part_")
        en = [l.rstrip("\n").split("\t", 1) for l in open(src, encoding="utf-8")]
        tr = [l.rstrip("\r\n").split("\t", 1) for l in open(part, encoding="utf-8")]
        if [k for k, _ in en] != [k for k, *_ in tr]:
            print("%s: keys differ" % part)
            problems += 1
            continue
        for (key, e), (_, t) in zip(en, tr):
            if sorted(TOKENS.findall(e)) != sorted(TOKENS.findall(t)):
                print("%s: tokens %s -> %s" % (key, TOKENS.findall(e), TOKENS.findall(t)))
                problems += 1
            b, bad = game_bytes(t.replace("\\n", "\n"), russian)
            if bad:
                print("%s: characters the fonts lack: %r" % (key, "".join(bad)))
                problems += 1
            if key.startswith("@") and len(b) > 255:
                print("%s: %d bytes, past a subtitle's slot" % (key, len(b)))
                problems += 1
            if not key.startswith("@") and key in rows:
                need = len(key) + 1 + len(field(b.decode("latin-1"))) + 4
                if need > rows[key]:
                    print("%s: row of %d, room %d" % (key, need, rows[key]))
                    problems += 1
            lines.append("%s\t%s" % (key, t))
    print(len(lines), "lines,", problems, "problems")
    if write or not problems:
        head = ["# Finest Hour Remastered: the game's text in %s (src/game/language.cpp). The port's own translation:" % language,
                '# "$key<tab>text" for the string tables\' rows, "@<hash><tab>text" for the subtitles and the levels\' text',
                "# (the hash of the English they replace), \\n a line break. UTF-8.", ""]
        with open(out, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(head + lines) + "\n")
        print("written", out)


if __name__ == "__main__":
    main()
