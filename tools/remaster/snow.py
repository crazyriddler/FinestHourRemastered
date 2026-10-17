"""The snowy ground's textures (their names: snow grounds, snowy roads, banks, drifts, ploughed and trodden snow; not
walls, trees, vehicles, uniforms) listed in the pack's snow.txt: the runtime leaves footprints in them (Snow Tracks).

    python -I snow.py <extract folder> <pack>
"""
import json
import os
import re
import sys

GROUND = re.compile(r"snowground|snowbase|snow_?base|ground_snow|road_snow|rubble_snow|snow_rocky|snow_bank|snow_even|"
                    r"snow_gray|snow_ground|snow_grassy|tiretracks|snow_plowed|snow_skirt|snow_tracks|snow_cliff|"
                    r"concrete_?snow|^snow$", re.I)
NOT = re.compile(r"wall|tree|bark|bush|leaves|billboard|train|truck|jacket|sandbag|wood|board|door|metal|hay|trim|"
                 r"clumps|flake|beam|tarp|plank|brick", re.I)


def main():
    src, pack = sys.argv[1], sys.argv[2]
    idx = json.load(open(os.path.join(src, "index.json")))
    ids = []
    for rid, v in sorted(idx.items()):
        names = v["names"]
        if any(GROUND.search(n) for n in names) and not any(NOT.search(n) for n in names):
            ids.append(rid)
            print(rid, " ".join(names))
    with open(os.path.join(pack, "snow.txt"), "w") as f:
        f.write("# the snowy ground (tools/remaster/snow.py): footprints (Snow Tracks)\n")
        f.write("\n".join(ids) + "\n")
    print("%d snowy textures" % len(ids))


if __name__ == "__main__":
    main()
