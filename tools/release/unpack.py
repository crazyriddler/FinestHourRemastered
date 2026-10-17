"""The release's own files for a build from source: the remaster pack, the shaders' recipes and the default settings,
taken from the GitHub release's payload (FinestHourRemastered.index and its .data parts) into the build's folder,
beside the CODFH.exe built. With the release's toolchain that build is the release, byte for byte (the exe is checked
against the payload's).

    python tools/release/unpack.py <folder with the release's downloads> [--to out/build/release]
"""
import argparse
import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("release")
    ap.add_argument("--to", default=os.path.join(ROOT, "out", "build", "release"))
    a = ap.parse_args()
    index = os.path.join(a.release, "FinestHourRemastered.index")
    lines = open(index, encoding="utf-8").read().splitlines()
    magic, parts, total = lines[0].split()
    if magic != "FHR1":
        sys.exit("not a Finest Hour Remastered index")
    names = [os.path.join(a.release, "FinestHourRemastered.data.%03d" % i) for i in range(1, int(parts) + 1)]
    for n in names:
        if not os.path.exists(n):
            sys.exit("missing " + n)
    stream = (open(n, "rb") for n in names)
    cur = next(stream)

    def read(n):
        nonlocal cur
        out = bytearray()
        while len(out) < n:
            chunk = cur.read(n - len(out))
            if not chunk:
                cur.close()
                cur = next(stream)
                continue
            out += chunk
        return bytes(out)

    done = 0
    for line in lines[1:]:
        size, path = line.split("\t")
        data = read(int(size))
        if path == "CODFH.exe":
            built = os.path.join(a.to, "CODFH.exe")
            same = os.path.exists(built) and hashlib.sha1(open(built, "rb").read()).digest() == hashlib.sha1(data).digest()
            print("CODFH.exe built %s the release's" % ("is" if same else "differs from"))
            continue  # (the build's own exe kept)
        dst = os.path.join(a.to, path)
        if path.startswith("remaster/") and os.path.islink(os.path.join(a.to, "remaster")):
            continue  # (a pack of one's own linked in: left alone)
        os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
        with open(dst, "wb") as f:
            f.write(data)
        done += 1
    print("%d files into %s" % (done, a.to))


if __name__ == "__main__":
    main()
