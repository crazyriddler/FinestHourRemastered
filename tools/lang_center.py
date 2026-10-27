"""The menus' rows a language of the port's own puts in place of the English: centred as the English, by measure.

The menus draw a row's text from a fixed left edge; the string tables centre each language's text with leading spaces
of their own (the English "    Single Player", the French "          Solo"). A translation's text is given its own:
the English's spaces, plus half the difference of the two texts' widths in the menu font's (fbld24) boxes, the
language's own letters' boxes where lang/<code>_fonts.bin gives them (tools/remaster/lang_fonts.py), in spaces.
The press-START rows are the typewriter's, its letters all one width: by count.

Also written: $pcSettings and $pcSettingsCaps, the main menu's PC Settings (in place of Multiplayer, game/language.cpp),
centred as the English Multiplayer.

    python -I tools/lang_center.py <game folder> lang/<x>.txt Russian|Polish|Portuguese
"""
import mmap
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from lang_merge import game_bytes  # noqa: E402

PC_SETTINGS = {"russian": ("Настройки ПК", "НАСТРОЙКИ ПК"), "polish": ("Opcje PC", "OPCJE PC"),
               "portuguese": ("Opções do PC", "OPÇÕES DO PC")}
TYPEWRITER = {"$pressStart", "$pressStartNGC", "$pressStartXbox"}


def english_rows(pak):
    """$key -> the English field of every string table row (with its spaces)"""
    data = open(pak, "rb").read()
    head = b"language,English,German,French,Italian,Spanish"
    out = {}
    at = data.find(head)
    while at >= 0:
        end = data.find(b"\0", at)
        for line in data[at:end].split(b"\n")[1:]:
            line = line.rstrip(b"\r")
            if not line.startswith(b"$") or b"," not in line:
                continue
            key, rest = line.split(b",", 1)
            if rest.startswith(b'"'):
                continue
            out.setdefault(key.decode("latin-1"), rest.split(b",", 1)[0].decode("latin-1"))
        at = data.find(head, at + 1)
    return out


def widths(pak, code):
    """the menu font's (fbld24) advance of each byte, the language's own letters' boxes in"""
    f = open(pak, "rb")
    m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    patched = {}
    fb = os.path.join(os.path.dirname(HERE), "lang", code + "_fonts.bin") if code else None
    if fb and os.path.exists(fb):
        b = open(fb, "rb").read()
        n, at = struct.unpack_from("<I", b, 16)[0], 20
        for _ in range(n):
            off, ln, kind = struct.unpack_from("<QII", b, at)
            at += 16
            if kind == 0:
                patched[off] = b[at:at + ln]
            at += ln
    i = m.find(b"METRICS1")
    while i >= 0:
        raw = m[i:m.find(b"\0", i)]
        lines = raw.split(b"\r\n")
        if os.path.splitext(lines[1].split()[0].decode("latin-1"))[0] == "fbld24":
            out, pos = {}, i + sum(len(l) + 2 for l in lines[:3])
            for line in lines[3:]:
                line = patched.get(pos, line)
                p = line.split(b"#")[0].split()
                if len(p) >= 5:
                    c, x0, _, x1, _ = map(int, p[:5])
                    out[c] = x1 - x0 + 1
                pos += len(line) + 2
            return out
        i = m.find(b"METRICS1", i + 1)
    sys.exit("no fbld24 metrics")


def measure(text, w, language):
    b, _ = game_bytes(text, language == "russian", language == "polish")
    return sum(w.get(c, w.get(ord("n"), 10)) for c in b)


def pad(key, english, text, w, language):
    lead = len(english) - len(english.lstrip(" "))
    en = english.strip(" ")
    t = text.strip(" ")
    if key in TYPEWRITER:
        n = lead + (len(en) - len(t)) // 2
    else:
        n = round(lead + (measure(en, w, language) - measure(t, w, language)) / (2 * w[32]))
    return " " * max(0, n) + t


def center(game, path, language):
    language = language.lower()
    pak = os.path.join(game, "COD.PAK")
    code = {"russian": "ru", "polish": "pl"}.get(language)
    w = widths(pak, code)
    en = english_rows(pak)
    lines = open(path, encoding="utf-8").read().split("\n")
    out, changed = [], 0
    for l in lines:
        if l.startswith("$pcSettings"):
            continue
        key, tab, text = l.partition("\t")
        if tab and key in en and en[key].startswith(" "):
            new = pad(key, en[key], text, w, language)
            changed += new != text
            l = key + "\t" + new
        out.append(l)
        if tab and key == "$multiplayerCaps" and language in PC_SETTINGS:
            name, caps = PC_SETTINGS[language]
            out.append("$pcSettings\t" + pad("$multiplayer", en["$multiplayer"], name, w, language))
            out.append("$pcSettingsCaps\t" + caps)
    open(path, "w", encoding="utf-8", newline="\n").write("\n".join(out))
    print(path, changed, "rows centred")


def main():
    center(sys.argv[1], sys.argv[2], sys.argv[3])


if __name__ == "__main__":
    main()
