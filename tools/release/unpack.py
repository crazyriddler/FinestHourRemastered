"""The release's own files for a build from source: the remaster pack, the shaders' recipes and the default settings,
taken from the GitHub release's payload (FinestHourRemastered.index and its .data parts) into the build's folder,
beside the CODFH.exe built, then the latest update carried inside FinestHourRemasteredSetup.exe (or
FinestHourRemasteredUpdate.exe) over them. With the release's toolchain that build is the release, byte for byte (the
exe is checked against the update's, else the payload's).

    python tools/release/unpack.py <folder with the release's downloads> [--to out/build/release]
"""
import argparse
import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def read_patch(folder):
    """the latest update inside the installer or the updater (make_release.py): its files by path"""
    import ctypes
    import struct
    for name in ("FinestHourRemasteredSetup.exe", "FinestHourRemasteredUpdate.exe"):
        path = os.path.join(folder, name)
        if not os.path.exists(path):
            continue
        blob = open(path, "rb").read()
        if blob[-8:] != b"FHRPATCH":
            continue
        packed, raw = struct.unpack("<QQ", blob[-24:-8])
        cab = ctypes.windll.cabinet
        h = ctypes.c_void_p()
        cab.CreateDecompressor(5, None, ctypes.byref(h))
        out = ctypes.create_string_buffer(raw)
        got = ctypes.c_size_t()
        cab.Decompress.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t,
                                   ctypes.POINTER(ctypes.c_size_t)]
        if not cab.Decompress(h, blob[-24 - packed:-24], packed, out, raw, ctypes.byref(got)):
            sys.exit("the update in %s cannot be unpacked" % name)
        cab.CloseDecompressor(h)
        data = out.raw[:got.value]
        end = data.index(b"\nend\n") + 5
        files, at = {}, end
        for line in data[:end].decode("utf-8").splitlines():
            if line.startswith("file "):
                _, size, _, rel = line.split(" ", 3)
                files[rel] = data[at:at + int(size)]
                at += int(size)
        return files
    return {}


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

    patch = read_patch(a.release)
    done = 0
    for line in lines[1:]:
        size, path = line.split("\t")
        data = read(int(size))
        if path in patch:
            continue  # (the update's newer copy, below)
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
    for path, data in patch.items():
        if path == "CODFH.exe":
            built = os.path.join(a.to, "CODFH.exe")
            same = os.path.exists(built) and hashlib.sha1(open(built, "rb").read()).digest() == hashlib.sha1(data).digest()
            print("CODFH.exe built %s the release's (its latest update)" % ("is" if same else "differs from"))
            continue
        dst = os.path.join(a.to, path)
        os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
        with open(dst, "wb") as f:
            f.write(data)
        done += 1
    print("%d files into %s (%d of them the latest update's)" % (done, a.to, len(patch)))


if __name__ == "__main__":
    main()
