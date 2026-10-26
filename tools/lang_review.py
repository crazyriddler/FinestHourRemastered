"""A translation's review sheet for a reviewer, and back: each line's key, its English and its translation in blocks
("RU:" or "PL:" lines the only ones to change); then the settings overlay's (its English and translation). Read back, the
reviewed file's lines replace the parts' (work/<x>/part_N.tsv) and the overlay's sheet, for lang_merge.py and
lang_overlay.py.

    python -I tools/lang_review.py out work/ru work/ru/ui_ru.tsv review.txt [ru|pl]
    python -I tools/lang_review.py in work/ru work/ru/ui_ru.tsv review.txt [ru|pl]
"""
import glob
import os
import sys

# per language: its name, its lines' label, its letters, a word in it
LANGS = {
    "ru": ("Russian", "RU", "Russian (Ё/ё are fine), Latin",
           "Спасибо за проверку! Меняйте только строки «RU:» и пришлите файл целиком обратно в issue #34. Коды вида\n"
           "$ms$, %d, {SaveGameSize} и \\n оставьте как есть."),
    "pl": ("Polish", "PL", "Polish (ą ć ę ł ń ó ś ź ż), Latin (but not æ ê ñ œ ¥ £ ¿)",
           "Dziękuję za pomoc! Zmieniaj tylko wiersze „PL:” i odeślij cały plik w issue #34. Kody takie jak $ms$,\n"
           "%d, {SaveGameSize} i \\n zostaw bez zmian."),
}


def head(lang):
    name, label, letters, native = LANGS[lang]
    title = "Finest Hour Remastered - %s translation review" % name
    return """%s
%s

Thank you for reviewing! Each entry below has the English and the %s. Change only the "%s:" lines, then send
the whole file back (attach it to issue #34).

Please keep, exactly as they are:
- $xx$ codes (button pictures), for example $ms$, $pa$, $ud$;
- %%d, %%n, %%s and {SaveGameSize} (numbers and sizes the game puts in);
- \\n (a line break) and leading spaces;
- the "# key" lines and the "EN:" lines.
Letters: %s, digits and « » — – … ' ". Subtitles (keys starting with @) should stay under
about 240 characters; menu items short, as they sit in fixed places.

%s

""" % (title, "=" * len(title), name, label, letters, native)


def main():
    mode, folder, ui, path = sys.argv[1:5]
    lang = sys.argv[5] if len(sys.argv) > 5 else "ru"
    label = LANGS[lang][1]
    if mode == "out":
        out = [head(lang), "GAME TEXT\n=========\n"]
        for src in sorted(glob.glob(os.path.join(folder, "src_*.tsv"))):
            en = [l.rstrip("\n").split("\t", 1) for l in open(src, encoding="utf-8")]
            tr = [l.rstrip("\r\n").split("\t", 1) for l in open(src.replace("src_", "part_"), encoding="utf-8")]
            for (k, e), (_, t) in zip(en, tr):
                out.append("# %s\nEN: %s\n%s: %s\n" % (k, e, label, t))
        out.append("\nSETTINGS MENU\n=============\n")
        for i, line in enumerate(open(ui, encoding="utf-8")):
            e, t = line.rstrip("\r\n").split("\t", 1)
            out.append("# ui%d\nEN: %s\n%s: %s\n" % (i + 1, e, label, t))
        open(path, "w", encoding="utf-8", newline="\n").write("\n".join(out))
        return
    # in: the reviewed file's translation lines by key
    got, key = {}, None
    for line in open(path, encoding="utf-8-sig"):
        line = line.rstrip("\r\n")
        if line.startswith("# "):
            key = line[2:].strip()
        elif line.startswith(label + ":") and key:
            got[key] = line[len(label) + 2:] if line[len(label) + 1:len(label) + 2] == " " else line[len(label) + 1:]
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
