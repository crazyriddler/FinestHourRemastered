"""Release crash dump addresses named: CODFH.exe+0xRVA through a linker map of the same build (lld /MAP).

    python tools/symbolize.py <CODFH.map> <rva hex>...   (or the output of tools/minidump.py on stdin)
"""
import bisect
import re
import sys


def load(path):
    syms = []
    for line in open(path, encoding="latin1"):
        m = re.match(r"\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s", line)
        if m:
            syms.append((int(m.group(2), 16) - 0x140000000, m.group(1)))
    syms.sort()
    return syms


def main():
    syms = load(sys.argv[1])
    keys = [a for a, _ in syms]
    text = " ".join(sys.argv[2:]) if len(sys.argv) > 2 else sys.stdin.read()
    for line in text.splitlines() if len(sys.argv) <= 2 else sys.argv[2:]:
        m = re.search(r"CODFH\.exe\+0x([0-9A-Fa-f]+)", line) or re.match(r"(?:0x)?([0-9A-Fa-f]+)$", line.strip())
        if not m:
            print(line)
            continue
        rva = int(m.group(1), 16)
        i = bisect.bisect_right(keys, rva) - 1
        name = syms[i][1] if i >= 0 else "?"
        print(f"{line.rstrip()}  =>  {name}+0x{rva - syms[i][0]:X}")


if __name__ == "__main__":
    main()
