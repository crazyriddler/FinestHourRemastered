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
# a flag taken for skin: light polygons over it where its faces met (the Matmata headquarters', level 14)
SURFACES["e609c975fc6d28dd"] = "fabric"  # flagwhite
# the flags and banners the classifier took for paper, metal, skin, the ground or the interface: cloth (the others
# with "flag" in their name are cloth already, or the HUD's and the objectives' icons, the flag pole's base, the
# sprite of a soldier running with a flag)
SURFACES.update({rid: "fabric" for rid in (
    "a12e6522e0b55496",  # L1_1_NaziFlag
    "269d01b5baea2e1f",  # L1_1_RussianFlag
    "cfbecb42b67bcb7c",  # L3_5_USFlag
    "fd12526f25f29d97",  # VT_Steamboat_flag_png
    "c46666ce48d3943c",  # _aNaziFlagFabric
    "0038ed62c95a975f",  # fabric_luftwaffe_flag_yellow
    "4344a19edd992585",  # torn_flag_naz_png
)})
# weapons' textures taken for skin (the Grease Gun's back, the mounted MG42's): an atlas, as the others (pores on a gun)
SURFACES.update({"7d02195d666f7ad2": "atlas", "79b7601760051594": "atlas"})
# the cacti (Tunisia): foliage sways in the wind, a cactus stands firm; their maps kept, not swaying
SURFACES.update({rid: "atlas" for rid in (
    "0541f938dfe5123e", "6db9e7e32caec00e",  # cactus1
    "77edf3730a6bce64", "ebdbd35568e95d48",  # cactus2
    "322023d4ab544363",  # cactus3
    "961df8765d1c6bfb", "c087f0b6e3b37d91",  # cactus4
)})

# the weapons' textures (in hand, picked up, mounted, on a tripod): wood and metal in one picture, taken for an atlas, metal
# or wood whole: their detail by each pixel's colour (-2: game/renderer's wood where warm, metal elsewhere), their
# class kept
BY_COLOUR = (r"^(PUT|MWT_|Tripod_MG42$|mg42$|gun$|gun50cal$|VT_Flak38_gun$|"
             r"(Bren|DP_?|Enfield|G43|Grease|Kar|Luger|M1Garand|MP40_?|Mg42|Mosin|NagantGI|PPSH|Springfield|Sten|Thompson|"
             r"BAR|Garand|Maxim|M1919)(Back|Front|back|front)$)")


# the vehicles (tanks, half-tracks, trucks, planes, trains, their tracks) and radios: taken for the ground, an atlas, the
# interface, skin, concrete, cloth..., most without metal's detail map: metal's (3), their class (their light) kept; their
# wooden parts wood's (1); not their decals, glass, smoke or shadows
METAL = r"^(VT_|VM_|VTrack|radio|L2_1_Radio|L1_1_Bullhorn|L1_5_binoculars|truck_|Russian_car|train_)"
NOT_METAL = r"(decal|_png$|shadow|windshield|glass|window|propeller|smoke|damage|name|margie)"
WOODEN = r"(wood|boxcar|Rowboat|seat|crate|petrol_car)"
NO_DETAIL = {"glass", "effect", "sky", "ui", "foliage", "paper"}
# no parallax on the walls' cut faces about windows and doors (jambs, frames, sills, the fractured thickness of a wall):
# narrow faces seen from a step away, where the walk read as streaks (the user's choice, F9 of 2026-11-01); their
# relief stays
# parallax depths set by hand (the user's F9 marks)
POM_SET = {
    "41b121db8b47d5ab": 0.015,  # _RegDirtGround (Matmata's ground, level 14): its gravel stood up in lumps
}
# the snowy roads that are mostly mud (3_3_road_snowhalf, under 20% snow: level 30): "mud" in snow.txt, their
# footprints kept but the ground's relief, detail and parallax, deeper than the ground's own (0.1: their ruts and
# frozen lumps standing out, the user's choice)
MUD = ("40225ae3f1dbfaf3", "4f7a2174b2497216", "157361a6b993eec9", "7dca9f9c99efc813",
       "133dc7d18103e6f2", "d5045491db427700", "e338d662ac42658d", "adf05cfcd3491411")
POM_SET.update({rid: 0.1 for rid in MUD})
NO_POM = r"(WallFracture|WindowFrame|Window_Frame|windowframe|WindowSill|doorjamb|doorframe|DoorwayBlend|Dooredge|side_frame|sparkasse_frame)"


def by_name(src, pattern):
    """texture ids by their names in the extract's index: name -> id"""
    import re
    index = json.load(open(os.path.join(src, "index.json")))
    return {rid: v["names"][0] for rid, v in index.items()
            if isinstance(v, dict) and v.get("names") and any(re.search(pattern, n) for n in v["names"])}


def by_colour(src):
    """the weapons' texture ids (BY_COLOUR)"""
    return list(by_name(src, BY_COLOUR))


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
    # the review of every texture's material (review.tsv: by eye, its name and its colours), before the rules by hand
    # below (they win). A surface's class set with its maps kept; one that is not a surface (an effect, the sky, the
    # interface) loses its maps; a surface the pack made none for is listed for generate.py --only. Two materials in
    # one texture: the detail by colour (wood_metal -2, wood_masonry -3)
    review = os.path.join(os.path.dirname(os.path.abspath(__file__)), "review.tsv")
    no_maps = []
    if os.path.exists(review):
        changed = 0
        for line in open(review, encoding="utf-8"):
            p = line.rstrip("\n").split("\t")
            if len(p) < 3 or line.startswith("#") or p[0] not in mats:
                continue
            rid, cls, mix = p[0], p[1], p[2]
            old = mats[rid]
            new = {**old, **materials.params(cls), "cls": cls}
            surface = materials.params(cls)["pbr"]
            if not surface:
                new["pbr"] = False
                for kind in ("n", "m"):
                    f = os.path.join(pack, "textures", "%s.%s.dds" % (rid, kind))
                    if os.path.exists(f):
                        os.remove(f)
            elif not old.get("pbr") or not os.path.exists(os.path.join(pack, "textures", rid + ".n.dds")):
                no_maps.append(rid)
                new["pbr"] = False  # (until its maps are made)
            if mix == "wood_metal" and new["pbr"]:
                new["detail"] = -2
            elif mix == "wood_masonry" and new["pbr"]:
                new["detail"] = -3
            if new != old:
                mats[rid] = new
                if rid in classes:
                    classes[rid]["class"] = cls
                REWRITE.add(rid)
                changed += 1
        print("review: %d changed, %d surfaces without maps" % (changed, len(no_maps)))
        open(os.path.join(pack, "review_no_maps.txt"), "w").write("\n".join(no_maps) + "\n")
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
    weapons = 0
    for rid in by_colour(src):
        if rid in mats and mats[rid].get("pbr"):
            mats[rid] = {**mats[rid], "detail": -2}
            REWRITE.add(rid)
            weapons += 1
    print("weapons' detail by colour:", weapons)
    import re
    metal = wooden = 0
    for rid, name in by_name(src, METAL).items():
        m = mats.get(rid)
        if not m or not m.get("pbr") or m.get("cls") in NO_DETAIL or re.search(NOT_METAL, name, re.I) or m.get("detail") == -2:
            continue
        wood = bool(re.search(WOODEN, name, re.I))
        mats[rid] = {**m, "detail": 1 if wood else 3}
        REWRITE.add(rid)
        wooden += wood
        metal += not wood
    print("vehicles' and radios' detail: %d metal, %d wood" % (metal, wooden))
    flat = 0
    for rid in by_name(src, NO_POM):
        if rid in mats and mats[rid].get("pom", 0) > 0:
            mats[rid] = {**mats[rid], "pom": 0.0}
            REWRITE.add(rid)
            flat += 1
    print("no parallax on windows' and doors' edges:", flat)
    snow_txt = os.path.join(pack, "snow.txt")
    if os.path.exists(snow_txt):
        lines = []
        for line in open(snow_txt).read().splitlines():
            rid = line.split(" ", 1)[0]
            lines.append(rid + " mud" if rid in MUD else line)
        open(snow_txt, "w").write("\n".join(lines) + "\n")
    for rid, pom in POM_SET.items():
        if rid in mats:
            mats[rid] = {**mats[rid], "pom": pom}
            REWRITE.add(rid)
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
