"""A translation's review sheet for a reviewer, and back: each line's key, its English and its translation in blocks
("RU:" lines the only ones to change); then the settings overlay's (its English and translation). Read back, the
reviewed file's lines replace the parts' (work/<x>/part_N.tsv) and the overlay's sheet, for lang_merge.py and
lang_overlay.py.

    python -I tools/lang_review.py out work/ru work/ru/ui_ru.tsv review.txt
    python -I tools/lang_review.py in work/ru work/ru/ui_ru.tsv review.txt
"""
import glob
import os
import sys

HEAD = """Finest Hour Remastered - Russian translation review
====================================================

Thank you for reviewing! Each entry below has the English and the Russian. Change only the "RU:" lines, then send
the whole file back (attach it to issue #34).

Please keep, exactly as they are:
- $xx$ codes (button pictures), for example $ms$, $pa$, $ud$;
- %d, %n, %s and {SaveGameSize} (numbers and sizes the game puts in);
- \\n (a line break) and leading spaces;
- the "# key" lines and the "EN:" lines.
Letters: Russian (Ё/ё are fine), Latin, digits and « » — – … ' ". Subtitles (keys starting with @) should stay under
about 240 characters; menu items short, as they sit in fixed places.

Спасибо за проверку! Меняйте только строки «RU:» и пришлите файл целиком обратно в issue #34. Коды вида $ms$,
%d, {SaveGameSize} и \\n оставьте как есть.

"""


def main():
    mode, folder, ui, path = sys.argv[1:5]
    if mode == "out":
        out = [HEAD, "GAME TEXT\n=========\n"]
        for src in sorted(glob.glob(os.path.join(folder, "src_*.tsv"))):
            en = [l.rstrip("\n").split("\t", 1) for l in open(src, encoding="utf-8")]
            tr = [l.rstrip("\r\n").split("\t", 1) for l in open(src.replace("src_", "part_"), encoding="utf-8")]
            for (k, e), (_, t) in zip(en, tr):
                out.append("# %s\nEN: %s\nRU: %s\n" % (k, e, t))
        out.append("\nSETTINGS MENU\n=============\n")
        for i, line in enumerate(open(ui, encoding="utf-8")):
            e, t = line.rstrip("\r\n").split("\t", 1)
            out.append("# ui%d\nEN: %s\nRU: %s\n" % (i + 1, e, t))
        open(path, "w", encoding="utf-8", newline="\n").write("\n".join(out))
        return
    # in: the reviewed file's RU lines by key
    got, key = {}, None
    for line in open(path, encoding="utf-8-sig"):
        line = line.rstrip("\r\n")
        if line.startswith("# "):
            key = line[2:].strip()
        elif line.startswith("RU:") and key:
            got[key] = line[3:].lstrip(" ") if not line[3:4] == " " else line[4:]
            key = None
    changed = 0
    for part in sorted(glob.glob(os.path.join(folder, "part_*.tsv"))):
        rows = [l.rstrip("\r\n").split("\t", 1) for l in open(part, encoding="utf-8")]
        new = []
        for k, t in rows:
            if k in got and got[k] != t:
                changed += 1
                t = got[k]
            new.append("%s\t%s" % (k, t))
        open(part, "w", encoding="utf-8", newline="\n").write("\n".join(new) + "\n")
    rows = [l.rstrip("\r\n").split("\t", 1) for l in open(ui, encoding="utf-8")]
    new = []
    for i, (e, t) in enumerate(rows):
        k = "ui%d" % (i + 1)
        if k in got and got[k] != t:
            changed += 1
            t = got[k]
        new.append("%s\t%s" % (e, t))
    open(ui, "w", encoding="utf-8", newline="\n").write("\n".join(new) + "\n")
    print(changed, "lines changed")


if __name__ == "__main__":
    main()
