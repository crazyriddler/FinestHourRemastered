"""Classes the image classifier got wrong, set by hand: effect sprites it took for surfaces (the dirt bullets throw up
read as an atlas and as skin) get surface maps, and lit as walls they draw as dark, hard-edged blots. Rewrites the
class in the extract's classes.json, the pack's materials.json and materials.txt, and removes the
surface maps.

    python -I reclass.py <extract folder> <pack folder>
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import materials  # noqa: E402

REWRITE = set()  # the materials.txt lines to write anew
OVERRIDES = {
    "3ef5763b805afcb0": "effect",  # dirtgeyser2
    "9d1e433f62fe9d40": "effect",  # dirtgeyser3
    "f778fa6ae940992f": "effect",  # stormcloud
    # night skies taken for skin: lit as surfaces, each face of the sky's box shaded by its own normal (level 11)
    "6074861bd653df6e": "sky",  # clouds above
    "e5591059dc0ab911": "sky",  # horizon
    "06a5db98ebbb9004": "sky",
    "31dbdbab9d20ffe0": "sky",
    "9a7b62dcbe01f35a": "sky",
    "72ca66321f344ce6": "sky",  # night clouds taken for an atlas (levels 14, 15)
    # pure black (the night sky's backdrop, holes, bores) taken for metal and the like: lit as metal they mirrored the
    # sky and the lights about them, a bright panel where the game has black (levels 14, 15's sky). Unlit, as the game.
    "f2a0c7d180a458ff": "sky",
    "01ff52bfc9fb5b88": "sky",
    "69039dcd0c9ca70c": "sky",
    "8e008c2f40bd71e3": "sky",
    "95809efd5ab44f84": "sky",
    "a55b15a3f3e0cd0b": "sky",
    "a622776d60fed806": "sky",
    "a7f25672f49166be": "sky",
    "b9c7ecc72fcfd421": "sky",
    "d63f67e622630665": "sky",
    "dbfeb4ce5701b825": "sky",
    "dd2e8ae211fb8e79": "sky",
}

# left as the game has them, not replaced: their line and maps out of the pack
DROP = set()

# classes set by hand with their surface maps kept: the snowy ground taken for wood, cloth, metal or an atlas (its
# sheen on the snow of levels 8 and 12; the tyre tracks as metal glinted in a row)
SURFACES = {rid: "ground" for rid in (
    "133dc7d18103e6f2", "157361a6b993eec9", "1acb9c1031516ef2", "360fbf813d4f4ae7", "40225ae3f1dbfaf3",
    "476d4f8992544ed8", "6f611ee816077cc9", "a3b61a54400165d5", "a62413a152a9fca3", "adf05cfcd3491411",
    "d5045491db427700", "e338d662ac42658d", "e43a7edc7447ad2d", "ec1438925c5ea6f5", "f4205acb11192cd1")}
# the first-person arms taken for cloth or an atlas: the cloth's detail map tiled over the hand drew dark streaks
# like dirt on it (the American GI's, the tank crews', the Soviet sniper's): skin, as the other arms
SURFACES.update({rid: "skin" for rid in (
    "7485dd2b5b2c4c6e",  # AT_USGI
    "aa703ba39a4524ac",  # AT_UStanker
    "207db7d39724d96d",  # AT_SovSniper
    "8f56ced3e24d5add",  # Arm_TankComm
)})


def glass_frames(src, classes):
    """'glass' that is a window's frame: its panes cut out (clear), the rest opaque. What shows is the frame (wood,
    painted), never glass: lit as glass it mirrored the bright street outside onto dark rooms"""
    import numpy as np
    from PIL import Image
    out = []
    for rid, c in classes.items():
        if c["class"] != "glass":
            continue
        p = os.path.join(src, rid + ".png")
        if not os.path.exists(p):
            continue
        a = np.asarray(Image.open(p).convert("RGBA"))[..., 3]
        if (a < 26).mean() > 0.15 and (a > 230).mean() > 0.15:
            out.append(rid)
    return out


def main():
    src, pack = sys.argv[1], sys.argv[2]
    classes_path = os.path.join(src, "classes.json")
    classes = json.load(open(classes_path))
    mats_path = os.path.join(pack, "materials.json")
    mats = json.load(open(mats_path))
    # window frames: wood, their surface maps kept
    for rid in glass_frames(src, classes):
        classes[rid]["class"] = "wood"
        if rid in mats:
            mats[rid] = {**mats[rid], **materials.params("wood"), "cls": "wood"}
            REWRITE.add(rid)
    for rid, cls in OVERRIDES.items():
        REWRITE.add(rid)
        if rid in classes:
            classes[rid]["class"] = cls
        if rid in mats:
            old = mats[rid]
            mats[rid] = {**old, **materials.params(cls), "cls": cls, "pbr": False}
        for kind in ("n", "m"):
            p = os.path.join(pack, "textures", "%s.%s.dds" % (rid, kind))
            if os.path.exists(p):
                os.remove(p)
    for rid, cls in SURFACES.items():
        REWRITE.add(rid)
        if rid in classes:
            classes[rid]["class"] = cls
        if rid in mats:
            mats[rid] = {**mats[rid], **materials.params(cls), "cls": cls}
    for rid in DROP:
        mats.pop(rid, None)
        for kind in ("c", "n", "m"):
            p = os.path.join(pack, "textures", "%s.%s.dds" % (rid, kind))
            if os.path.exists(p):
                os.remove(p)
    json.dump(classes, open(classes_path, "w"), indent=1, sort_keys=True)
    json.dump(mats, open(mats_path, "w"), indent=1, sort_keys=True)
    # materials.txt: those lines alone (others, as hud.py's, are added to it by hand)
    txt = os.path.join(pack, "materials.txt")
    out = []
    for line in open(txt).read().splitlines():
        rid = line.split(" ", 1)[0]
        if rid in DROP:
            continue
        if rid in REWRITE:
            m = mats[rid]
            line = "%s %s %d %.3f %.4f %.3f %d %d" % (rid, m["cls"], int(m["pbr"]), m["normal"], m["pom"], m["spec"], m["detail"], m["tiles"])
        out.append(line)
    open(txt, "w").write("\n".join(out) + "\n")
    print("reclassed:", len(OVERRIDES))


if __name__ == "__main__":
    main()
