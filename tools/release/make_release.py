"""Finest Hour Remastered's releases, from the build and the remaster pack:

  release/portable/Finest Hour Remastered/   the player's own: ready to play from anywhere (the exe, the pack, every
                                             shader prepared for this PC, the default settings, the game's files from
                                             --game under assets/)
  release/source/FinestHourRemastered/       the repository's committed files (git archive): the code, the tools, the
                                             installer; no game files, no remaster pack, no work files
  release/github/                            the public one, the v1.0 release: the base payload (fixed since: its
                                             FinestHourRemastered.index and .data parts as published, listed in
                                             base_manifest.txt) and the installer, which carries the latest update
                                             inside it (every file changed since the base) and applies it after
  release/update/                            the latest update release: the same exe as FinestHourRemasteredUpdate.exe,
                                             the updater of an installation of any earlier version

    python tools/release/make_release.py [--game ../COD] [--only portable|github|source] [--version text]
    python tools/release/make_release.py --only github --rebase   (a new base payload: the parts written again)
"""
import argparse
import ctypes
import datetime
import hashlib
import io
import os
import shutil
import stat
import subprocess
import struct
import sys
import tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.join(ROOT, "out", "build", "release")
PACK = os.path.join(ROOT, "work", "remaster", "pack")
OUT = os.path.join(os.path.dirname(ROOT), "release")
EXE = "CODFH.exe"  # its own name: the game starts itself again under it (a title launch)
PART = 1900 * 1024 * 1024

# what of the pack ships (the work_* folders are the tools' own)
PACK_PARTS = ["materials.txt", "shadows.txt", "snow.txt", "grains.txt", "details", "textures", "ui", "meshes"]
# the shader cache: the recipes, compiled shaders and pipeline descriptions go anywhere (the game makes the
# pipelines from them at its first start); the pipeline library (pipelines.bin, .id) is this PC driver's
CACHE_ANY = ["recipes.bin", "blobs.bin", "passes.bin", "psos2.bin", "shadows2.bin"]

# the settings a release starts with: the author's, the display's size chosen by the game (0: the desktop's), English
DEFAULTS = """anisotropic_filtering = 16
brightness = 100
button_prompts = 0
display_mode = 'borderless'
fov = 0
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
    # the port's translations (lang/pt.txt: Portuguese, src/game/language.cpp)
    lang = os.path.join(ROOT, "lang")
    items += [(os.path.join(lang, f), os.path.join("lang", f)) for f in sorted(os.listdir(lang))]
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


# the update: every file the base payload lacks or has otherwise, and those that changed between the published
# versions of the base (the exe, the shaders, the readme and licences: an installation of any earlier version is
# brought up to date by them); the base's other files are checked by size; the first release's exe is deleted
ALWAYS = ("CODFH.exe", "d3dcompiler_47.dll", "README.md", "LICENSE.txt", "THIRD_PARTY.txt")
DELETE = ("FinestHourRemastered.exe",)
# files once shipped and no longer (the remastered meshes held back for review: tools/meshes/scope.py), taken out of
# an installation that has them
RETIRED = os.path.join(os.path.dirname(os.path.abspath(__file__)), "retired_files.txt")
KEEP = ("codfh.toml",)  # the player's own settings: never written by an update


def sha1_of(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


def base_manifest():
    out = {}
    for line in open(os.path.join(ROOT, "tools", "release", "base_manifest.txt"), encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        sha, size, path = line.rstrip("\r\n").split(" ", 2)
        out[path] = (sha, int(size))
    return out


def lzms(data, compress):
    """Windows' compression API (cabinet.dll), LZMS: what the installer unpacks with"""
    cab = ctypes.windll.cabinet
    h = ctypes.c_void_p()
    make = cab.CreateCompressor if compress else cab.CreateDecompressor
    if not make(5, None, ctypes.byref(h)):
        sys.exit("cabinet: no LZMS")
    run = cab.Compress if compress else cab.Decompress
    run.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    size = ctypes.c_size_t()
    run(h, data, len(data), None, 0, ctypes.byref(size))
    out = ctypes.create_string_buffer(size.value)
    if not run(h, data, len(data), out, size.value, ctypes.byref(size)):
        sys.exit("cabinet: failed")
    (cab.CloseCompressor if compress else cab.CloseDecompressor)(h)
    return out.raw[:size.value]


def patch_files(exe):
    """an update's files, read from its exe (write_installer's tail): {path: sha1}"""
    with open(exe, "rb") as f:
        data = f.read()
    if data[-8:] != b"FHRPATCH":
        sys.exit("%s: no update inside" % exe)
    packed, _ = struct.unpack("<QQ", data[-24:-8])
    raw = lzms(data[-24 - packed:-24], False)
    out = {}
    for line in raw[:raw.index(b"\nend\n") + 5].decode("utf-8").splitlines():
        if line.startswith("file "):
            _, sha, path = line[5:].split(" ", 2)
            out[path] = sha
    return out


def build_patch(items, version, undo=None):
    """the update over the base payload: (packed bytes, unpacked size, what it holds). undo: earlier updates' files
    ({path: set of sha1}, a withdrawn one's or the one published now): the ones they added that this version has not
    are deleted, the ones they changed are written as this version has them (the base's own too: a file back as the
    base has it is only checked by its size, and one an update changed kept its size), so an installation they
    updated comes out as this version"""
    base = base_manifest()
    undo = undo or {}
    head, data, files = ["FHRU 1", "version " + version], [], []
    staged = {}
    for s, r in items:
        r = r.replace("\\", "/")
        staged[r] = s
    for r, s in staged.items():
        if r in KEEP:
            continue
        sha = sha1_of(s)
        if r in ALWAYS or r.startswith("shadercache/") or r not in base or base[r][0] != sha or (r in undo and any(u != sha for u in undo[r])):
            size = os.path.getsize(s)
            head.append("file %d %s %s" % (size, sha, r))
            files.append(r)
            with open(s, "rb") as f:
                data.append(f.read())
    retired = []
    if os.path.exists(RETIRED):
        retired = [l.strip() for l in open(RETIRED) if l.strip() and not l.startswith("#")]
    for r in list(DELETE) + [r for r in retired if r not in staged and r not in base]:
        head.append("delete " + r)
    for r in undo:
        if r not in staged and r not in KEEP and r not in base:
            head.append("delete " + r)
    for r, (sha, size) in base.items():
        if r in KEEP or r in files:
            continue
        if r not in staged:
            head.append("delete " + r)
        else:
            head.append("check %d %s" % (size, r))
    head.append("end")
    raw = ("\n".join(head) + "\n").encode("utf-8") + b"".join(data)
    return lzms(raw, True), len(raw), files


def write_installer(setup, packed, raw_size, to):
    with open(to, "wb") as o:
        with open(setup, "rb") as f:
            o.write(f.read())
        o.write(packed)
        o.write(struct.pack("<QQ", len(packed), raw_size) + b"FHRPATCH")


def github(version, rebase=False, undo=None):
    if not rebase:
        # the base stays as published: the installer and the updater, each with the latest update inside
        base = os.path.join(OUT, "github")
        os.makedirs(base, exist_ok=True)
        items = stage_list(cache_all=False)
        earlier = {}
        for exe in undo or []:
            for r, sha in patch_files(exe).items():
                earlier.setdefault(r, set()).add(sha)
        packed, raw_size, files = build_patch(items, version, earlier)
        setup = os.path.join(BUILD, "tools", "installer", "FinestHourRemasteredSetup.exe")
        write_installer(setup, packed, raw_size, os.path.join(base, "FinestHourRemasteredSetup.exe"))
        upd = os.path.join(OUT, "update")
        if os.path.exists(upd):
            remove_tree(upd)
        os.makedirs(upd)
        write_installer(setup, packed, raw_size, os.path.join(upd, "FinestHourRemasteredUpdate.exe"))
        shutil.copy2(os.path.join(ROOT, "tools", "release", "RELEASE_NOTES.md"), base)
        shutil.copy2(os.path.join(ROOT, "tools", "release", "UPDATE_NOTES.md"), upd)
        print("github: update %s: %d files (%.1f MB unpacked, %.1f MB packed) in the installer and the updater" % (
            version, len(files), raw_size / 2**20, len(packed) / 2**20))
        for r in files:
            print("   ", r)
        return
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
    ap.add_argument("--version", default=datetime.datetime.now().strftime("%Y-%m-%d %H:%M"),
                    help="the update's name, shown by the installer and the updater")
    ap.add_argument("--rebase", action="store_true", help="a new base payload (its parts written again)")
    ap.add_argument("--undo", action="append", help="an earlier update's exe (a withdrawn one, the one published now; "
                    "again for more): what it changed written as this version has it (patch_files, build_patch)")
    a = ap.parse_args()
    if a.only == "source":
        source()
        return
    if not os.path.exists(os.path.join(BUILD, "CODFH.exe")):
        sys.exit("build first")
    if a.only in (None, "portable"):
        portable(a.game)
    if a.only in (None, "github"):
        github(a.version, a.rebase, a.undo)
    if a.only is None:
        source()


if __name__ == "__main__":
    main()
