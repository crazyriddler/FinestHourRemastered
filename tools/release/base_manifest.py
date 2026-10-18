"""The base payload's manifest: what the published .data parts hold (they no longer change; updates ship inside the
installer, make_release.py). Each file's SHA-1, size and path, from the index and the parts, into base_manifest.txt.

    python tools/release/base_manifest.py <folder with the .index and .data parts>
"""
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def entries(folder):
    """(size, path, sha1) of each file in the payload, in its order"""
    lines = open(os.path.join(folder, "FinestHourRemastered.index"), encoding="utf-8").read().splitlines()
    magic, parts, _ = lines[0].split()
    assert magic == "FHR1"
    names = [os.path.join(folder, "FinestHourRemastered.data.%03d" % i) for i in range(1, int(parts) + 1)]
    stream = (open(n, "rb") for n in names)
    cur = next(stream)
    for line in lines[1:]:
        size, path = line.split("\t")
        left, h = int(size), hashlib.sha1()
        while left:
            chunk = cur.read(min(left, 1 << 22))
            if not chunk:
                cur.close()
                cur = next(stream)
                continue
            h.update(chunk)
            left -= len(chunk)
        yield int(size), path, h.hexdigest()


def main():
    folder = sys.argv[1]
    out = os.path.join(HERE, "base_manifest.txt")
    n = 0
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("# the base payload (the v1.0 .data parts): sha1 size path\n")
        for size, path, sha in entries(folder):
            f.write("%s %d %s\n" % (sha, size, path))
            n += 1
    print(n, "files ->", out)


if __name__ == "__main__":
    main()
