"""The port as it is handed out: dist/CODFH with the exe, the readme and licences, an empty assets folder for the
game, and the remaster pack (materials.txt, shadows.txt, details/, textures/, prompts/, ui/, shadercache/) beside the exe, hard linked from the pack so it
takes no more disk. Also links the pack beside the build's exe so it runs with it.

    python tools/package.py [--pack work/remaster/pack] [--zip]

--zip writes dist/CODFH.zip (the port) and dist/CODFH-remaster.zip (the pack, stored: DDS do not compress much).
"""
import argparse
import os
import shutil
import subprocess
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "out", "build", "release")


def link_tree(src, dst, names=None):
    """the files of src (or those named) hard linked into dst, copied where links cannot be made"""
    os.makedirs(dst, exist_ok=True)
    count = 0
    for name in names if names is not None else os.listdir(src):
        s, d = os.path.join(src, name), os.path.join(dst, name)
        if os.path.isdir(s):
            count += link_tree(s, d)
            continue
        if os.path.exists(d):
            if os.path.samefile(s, d):
                continue
            os.remove(d)
        try:
            os.link(s, d)
        except OSError:
            shutil.copy2(s, d)
        count += 1
    return count


def junction(link, target):
    if os.path.exists(link):
        return
    subprocess.run(["cmd", "/c", "mklink", "/J", link, target], check=True, capture_output=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pack", default=os.path.join(ROOT, "work", "remaster", "pack"))
    ap.add_argument("--zip", action="store_true")
    args = ap.parse_args()

    exe = os.path.join(BUILD, "CODFH.exe")
    if not os.path.exists(exe):
        raise SystemExit("build first: %s is missing" % exe)
    dist = os.path.join(ROOT, "dist", "CODFH")
    os.makedirs(os.path.join(dist, "assets"), exist_ok=True)
    shutil.copy2(exe, dist)
    shutil.copy2(os.path.join(ROOT, "README.md"), dist)
    shutil.copy2(os.path.join(ROOT, "sdk", "THIRD_PARTY.txt"), dist)
    shutil.copy2(os.path.join(ROOT, "sdk", "LICENSE"), os.path.join(dist, "LICENSE.txt"))

    pack = os.path.abspath(args.pack)
    has_pack = os.path.exists(os.path.join(pack, "materials.txt"))
    if has_pack:
        remaster = os.path.join(dist, "remaster")
        # the shadows' level settings come with the tools: the pack takes them when it has none
        if not os.path.exists(os.path.join(pack, "shadows.txt")):
            shutil.copy2(os.path.join(ROOT, "tools", "remaster", "shadows.txt"), pack)
        parts = [p for p in ["materials.txt", "shadows.txt", "details", "textures", "prompts", "ui", "shadercache"]
                 if os.path.exists(os.path.join(pack, p))]
        n = link_tree(pack, remaster, parts)
        print("remaster pack: %d files linked" % n)
        # the build's exe finds the pack beside it too
        junction(os.path.join(BUILD, "remaster"), pack)
    else:
        print("no remaster pack at %s: the port goes without it" % pack)

    if args.zip:
        out = os.path.join(ROOT, "dist")
        with zipfile.ZipFile(os.path.join(out, "CODFH.zip"), "w", zipfile.ZIP_DEFLATED) as z:
            for name in ["CODFH.exe", "README.md", "THIRD_PARTY.txt", "LICENSE.txt"]:
                z.write(os.path.join(dist, name), os.path.join("CODFH", name))
            z.writestr("CODFH/assets/", "")
        if has_pack:
            with zipfile.ZipFile(os.path.join(out, "CODFH-remaster.zip"), "w", zipfile.ZIP_STORED, allowZip64=True) as z:
                base = os.path.join(dist, "remaster")
                for dp, _, files in os.walk(base):
                    for f in files:
                        p = os.path.join(dp, f)
                        z.write(p, os.path.join("CODFH", "remaster", os.path.relpath(p, base)))
        print("zips in %s" % out)
    print("ready: %s" % dist)


if __name__ == "__main__":
    main()
