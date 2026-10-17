"""Finest Hour Remastered's releases, from the build and the remaster pack:

  release/portable/Finest Hour Remastered/   the player's own: ready to play from anywhere (the exe, the pack, every
                                             shader prepared for this PC, the default settings, the game's files from
                                             --game under assets/)
  release/source/FinestHourRemastered/       the repository's committed files (git archive): the code, the tools, the
                                             installer; no game files, no remaster pack, no work files
  release/github/                            the public one: the installer, FinestHourRemastered.index and the payload
                                             in parts under 1.9 GB (GitHub's limit is 2 GB a file): the exe, the pack,
                                             the shaders' recipes (prepared on the player's PC at its first start),
                                             the default settings, the readme and licences; no game files

    python tools/release/make_release.py [--game ../COD] [--only portable|github|source]
"""
import argparse
import io
import os
import shutil
import stat
import subprocess
import sys
import tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.join(ROOT, "out", "build", "release")
PACK = os.path.join(ROOT, "work", "remaster", "pack")
OUT = os.path.join(os.path.dirname(ROOT), "release")
EXE = "FinestHourRemastered.exe"
PART = 1900 * 1024 * 1024

# what of the pack ships (the work_* folders are the tools' own)
PACK_PARTS = ["materials.txt", "shadows.txt", "snow.txt", "grains.txt", "details", "textures", "ui"]
# the shader cache: the recipes, compiled shaders and pipeline descriptions go anywhere (the game makes the
# pipelines from them at its first start); the pipeline library (pipelines.bin, .id) is this PC driver's
CACHE_ANY = ["recipes.bin", "blobs.bin", "passes.bin", "psos2.bin", "shadows2.bin"]

# the settings a release starts with: the author's, the display's size chosen by the game (0: the desktop's), English
DEFAULTS = """anisotropic_filtering = 16
brightness = 100
button_prompts = 0
display_mode = 'borderless'
fov = 0
fps_cap = 60
fps_counter = false
invert_mouse = false
invert_stick_y = false
keyboard_mouse = true
language = 1
mouse_sensitivity = 100
pad_layout = 0
render_percent = 100
shadow_quality = 4
sharpening = 0
skip_intros = false
"""


def files_of(base, names):
    out = []
    for n in names:
        p = os.path.join(base, n)
        if os.path.isdir(p):
            for dp, _, fs in os.walk(p):
                for f in sorted(fs):
                    full = os.path.join(dp, f)
                    out.append((full, os.path.relpath(full, base)))
        elif os.path.exists(p):
            out.append((p, n))
    return out


def defaults_from_user():
    """the author's settings file, with the display's size left to the game, the language English and the default
    key bindings"""
    src = os.path.join(ROOT, "work", "user_codfh.toml")
    if not os.path.exists(src):
        return DEFAULTS
    lines = []
    for l in open(src, encoding="utf-8").read().splitlines():
        if l.startswith("["):  # the key bindings: the defaults (the file's names are the author's keyboard's)
            break
        k = l.split("=")[0].strip()
        if k in ("width", "height", "join_ip", "keys_version", "developer"):
            continue
        if k == "language":
            l = "language = 1"
        if k == "skip_intros":
            l = "skip_intros = false"  # (the publishers' logos and the opening movie shown, as the game first starts)
        lines.append(l)
    return "\n".join(lines) + "\n"


def stage_list(cache_all):
    """(source, path inside the install folder) of the port's own files"""
    items = [(os.path.join(BUILD, "CODFH.exe"), EXE)]
    # the shader compiler the game loads (part of Windows 10 and 11; the Windows SDK's redistributable copy beside the
    # exe all the same: no dependency left on the system's)
    sdk = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "Windows Kits", "10", "Redist", "D3D", "x64",
                       "d3dcompiler_47.dll")
    if not os.path.exists(sdk):
        sys.exit("the Windows SDK's d3dcompiler_47.dll is needed: " + sdk)
    items.append((sdk, "d3dcompiler_47.dll"))
    items += [(s, os.path.join("remaster", r)) for s, r in files_of(PACK, PACK_PARTS)]
    cache = os.path.join(BUILD, "shadercache")
    names = sorted(os.listdir(cache)) if cache_all else CACHE_ANY
    items += [(s, os.path.join("shadercache", r)) for s, r in files_of(cache, names)]
    for src, dst in ((os.path.join(ROOT, "README.md"), "README.md"), (os.path.join(ROOT, "sdk", "THIRD_PARTY.txt"), "THIRD_PARTY.txt"),
                     (os.path.join(ROOT, "sdk", "LICENSE"), "LICENSE.txt")):
        if os.path.exists(src):
            items.append((src, dst))
    return items


def remove_tree(path):
    """a folder gone, its read-only files too (git's packs in a build's _deps)"""
    def again(func, p, _):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    shutil.rmtree(path, onexc=again)


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def portable(game):
    base = os.path.join(OUT, "portable", "Finest Hour Remastered")
    if os.path.exists(base):
        remove_tree(base)
    items = stage_list(cache_all=True)
    for i, (s, r) in enumerate(items):
        copy(s, os.path.join(base, r))
    with open(os.path.join(base, "codfh.toml"), "w", encoding="utf-8") as f:
        f.write(defaults_from_user())
    n = 0
    for dp, _, fs in os.walk(game):
        for f in fs:
            s = os.path.join(dp, f)
            copy(s, os.path.join(base, "assets", os.path.relpath(s, game)))
            n += 1
    print("portable: %d port files, %d game files -> %s" % (len(items), n, base))


def github():
    base = os.path.join(OUT, "github")
    if os.path.exists(base):
        remove_tree(base)
    os.makedirs(base)
    items = stage_list(cache_all=False)
    toml = os.path.join(base, "_codfh.toml")
    with open(toml, "w", encoding="utf-8") as f:
        f.write(defaults_from_user())
    items.append((toml, "codfh.toml"))
    total = sum(os.path.getsize(s) for s, _ in items)
    # the payload: the files one after another, cut into parts
    part, written, out = 1, 0, None

    def next_part():
        nonlocal part, written, out
        if out:
            out.close()
        out = open(os.path.join(base, "FinestHourRemastered.data.%03d" % part), "wb")
        part += 1
        written = 0

    next_part()
    for s, _ in items:
        with open(s, "rb") as f:
            while True:
                chunk = f.read(min(8 << 20, PART - written))
                if not chunk:
                    break
                out.write(chunk)
                written += len(chunk)
                if written >= PART:
                    next_part()
    out.close()
    parts = part - 1
    last = os.path.join(base, "FinestHourRemastered.data.%03d" % parts)
    if os.path.getsize(last) == 0:
        os.remove(last)
        parts -= 1
    with open(os.path.join(base, "FinestHourRemastered.index"), "w", encoding="utf-8", newline="\n") as f:
        f.write("FHR1 %d %d\n" % (parts, total))
        for s, r in items:
            f.write("%d\t%s\n" % (os.path.getsize(s), r.replace("\\", "/")))
    os.remove(toml)
    setup = os.path.join(BUILD, "tools", "installer", "FinestHourRemasteredSetup.exe")
    shutil.copy2(setup, base)
    shutil.copy2(os.path.join(ROOT, "tools", "release", "RELEASE_NOTES.md"), base)
    print("github: %d files, %.2f GB in %d parts -> %s" % (len(items), total / 2**30, parts, base))


def source():
    base = os.path.join(OUT, "source", "FinestHourRemastered")
    if os.path.exists(base):
        remove_tree(base)
    os.makedirs(base)
    tar = subprocess.run(["git", "archive", "--format=tar", "HEAD"], cwd=ROOT, capture_output=True, check=True).stdout
    with tarfile.open(fileobj=io.BytesIO(tar)) as t:
        t.extractall(base, filter="data")
    n = sum(len(fs) for _, _, fs in os.walk(base))
    print("source: %d files -> %s" % (n, base))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=os.path.join(os.path.dirname(ROOT), "COD"))
    ap.add_argument("--only", choices=["portable", "github", "source"])
    a = ap.parse_args()
    if a.only == "source":
        source()
        return
    if not os.path.exists(os.path.join(BUILD, "CODFH.exe")):
        sys.exit("build first")
    if a.only in (None, "portable"):
        portable(a.game)
    if a.only in (None, "github"):
        github()
    if a.only is None:
        source()


if __name__ == "__main__":
    main()
