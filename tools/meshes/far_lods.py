"""The soldiers' distant versions (their LODs: a head of 49 indices where the near one has 920, a hand of 39 or 183
where the near one has 900) are left as the game has them. Remastered from so little they came out broken (a hand
and sleeve frayed into strips), and they are only drawn far off, where the console's look is the right one.

A distant version: a soldier's piece (its texture's name: heads, bodies, arms and hands, gear) whose original draw is
under half of the largest of its texture's, or under FAR_COUNT indices (a body's or gear's distant version may have
a texture of its own). Its teeth are drawn only close up and stay remastered. batch.py leaves them out; this removes
those already in the pack.

    python tools/meshes/far_lods.py [--pack work/remaster/pack/meshes] [--dry]
"""
import argparse
import collections
import glob
import json
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fhm  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SOLDIER = re.compile(r"^(HT|HM|BT|Bt|BM|AT_|Arm_|hand|snowy)")
TEETH = re.compile(r"Teeth", re.I)
FAR_COUNT = {"HT": 300, "HM": 300}  # heads: their near versions have 900 and more
FAR_COUNT_OTHER = 600               # bodies, arms, hands, gear: their near versions 730 and more


def names():
    path = os.path.join(ROOT, "work", "remaster", "extract", "index.json")
    if not os.path.exists(path):
        return {}
    return {k: (v.get("names") or [""])[0] for k, v in json.load(open(path)).items()}


# the main pieces (heads, bodies, arms, hands, gear), whose near versions are all large: under FAR_COUNT they are a
# distant one even with a texture of their own; the small pieces (collars, straps, hair, caps, patches) only by the
# share of their texture's largest
MAIN = re.compile(r"^(HT\d|HT_Uni|HT_Male|HM_|BT\d+\w*_(up|low|upper|lower)|BM|Bt50|AT_|Arm_|hand|snowy)", re.I)
SMALL = re.compile(r"collar|strap|hair|patch|hat|cap", re.I)


def far(name, count, largest):
    """a soldier's distant version, by its texture's name, its draw's index count and its texture's largest"""
    name = name.strip()
    if not SOLDIER.match(name) or TEETH.search(name):
        return False
    if count < 0.5 * largest:
        return True
    if not MAIN.match(name) or SMALL.search(name):
        return False
    return count < FAR_COUNT.get(name[:2], FAR_COUNT_OTHER)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pack", default=os.path.join(ROOT, "work", "remaster", "pack", "meshes"))
    ap.add_argument("--dry", action="store_true")
    a = ap.parse_args()
    tex_names = names()
    meshes = [(p, fhm.read(p)) for p in sorted(glob.glob(os.path.join(a.pack, "*.fhm")))]
    largest = collections.defaultdict(int)
    for _, m in meshes:
        largest[m.tex] = max(largest[m.tex], m.count)
    out = os.path.join(ROOT, "work", "remaster", "removed", "far_lods")
    n = 0
    for p, m in meshes:
        name = tex_names.get(f"{m.tex:016x}", "")
        if not far(name, m.count, largest[m.tex]):
            continue
        n += 1
        print(f"{os.path.basename(p)} {name.strip()} {m.count} of {largest[m.tex]}")
        if not a.dry:
            os.makedirs(out, exist_ok=True)
            shutil.move(p, os.path.join(out, os.path.basename(p)))
    print(f"{n} distant versions {'found' if a.dry else 'removed'}")


if __name__ == "__main__":
    main()
