"""The settings overlay's text in a language, as C++ (src/common/overlay_text_<x>.cpp), from a sheet of "English<TAB>
translation" lines (UTF-8): the English as overlay.cpp writes it.

    python -I tools/lang_overlay.py work/ru/ui_ru.tsv Russian ru > src/common/overlay_text_ru.cpp
"""
import sys


def c(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def main():
    sheet, language, code = sys.argv[1:4]
    rows = []
    for line in open(sheet, encoding="utf-8"):
        line = line.rstrip("\r\n")
        if "\t" in line:
            en, tr = line.split("\t", 1)
            rows.append((en, tr))
    out = ["// The settings overlay's %s (overlay_text.cpp has the other languages): English as written in overlay.cpp" % language,
           "// and pc_input.cpp, its %s here. UTF-8. Made by tools/lang_overlay.py." % language,
           '#include "common/overlay_text.h"', "", "namespace cod::overlay {", "",
           "const PtRow k%sRows[] = {" % code.capitalize()]
    for en, tr in rows:
        out.append('    {"%s", "%s"},' % (c(en), c(tr)))
    out += ["};", "const size_t k%sRowCount = sizeof(k%sRows) / sizeof(k%sRows[0]);" % ((code.capitalize(),) * 3), "",
            "}  // namespace cod::overlay", ""]
    sys.stdout.buffer.write("\n".join(out).encode("utf-8"))


if __name__ == "__main__":
    main()
