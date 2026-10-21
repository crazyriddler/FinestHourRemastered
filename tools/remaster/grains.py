"""The detail grain (Detail Textures) of the characters' textures by their names, over their material classes (which
the classifier often got wrong for uniforms: atlas, ground, skin, metal, ui; ground's pebbles read as dirt on a
coat): bodies' upper and lower halves (BTnn_up/upper/low/lower, BMnn_...) fabric; the first-person arms (AT_...) skin; caps and helmets (HTS..)
fabric or metal; heads (HT_...) none (their pores are Character Detail's). Writes <pack>/grains.txt: id kind.

    python -I grains.py <extract folder> <pack folder>
"""
import json
import os
import re
import sys

KINDS = ["stone", "brick", "concrete", "wood", "metal", "fabric", "ground", "roof", "foliage", "paper", "plaster",
         "skin", "snow", "grain", "none"]


def kind_of(name):
    n = name.strip().lower()
    if re.search(r"(^|_)hands?($|_)", n) or n.startswith("hand_") or n == "hand":
        return "skin"  # (hands: their pores, no grain)
    if re.match(r"^(bt|bm|lbt)\d+.*(_up|_upper|_low|_lower|upper|lower|_up_|_low_)", n) or re.match(r"^(bt|bm)\d+_", n):
        return "fabric"
    # the first-person arms (AT_..., Arm_...): one texture for the sleeve and the hand; a fabric grain over it drew
    # dark streaks like dirt across the hand: skin's (pores) over both
    if (re.match(r"^at_", n) and not any(s in n for s in ("flak", "wehr", "barrel"))) or n.startswith("arm_"):
        return "skin"
    if n.startswith("hts"):
        return "metal" if "helmet" in n else "fabric"
    if re.match(r"^ht_", n) or "head" in n:
        return "none"
    if n in ("snowupper", "snowylower") or n.startswith("deadrussian"):
        return "fabric"
    return None


def main():
    extract, pack = sys.argv[1], sys.argv[2]
    index = json.load(open(os.path.join(extract, "index.json")))
    out = []
    for rid, info in index.items():
        kinds = {kind_of(n) for n in info["names"]} - {None}
        if len(kinds) == 1:
            out.append((rid, kinds.pop(), info["names"][0]))
    with open(os.path.join(pack, "grains.txt"), "w") as f:
        f.write("# id kind (tools/remaster/grains.py): the detail grain of characters' textures over their class\n")
        for rid, k, n in sorted(out, key=lambda t: t[2].lower()):
            f.write("%s %s\n" % (rid, k))
    print(len(out), "textures")


if __name__ == "__main__":
    main()
