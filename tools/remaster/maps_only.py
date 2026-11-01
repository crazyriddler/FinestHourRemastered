"""The surface maps (normal, and roughness/height/cavity/specular) of textures the pack has none for, their colour left
as it is: the ones review.tsv made surfaces (reclass.py lists them in <pack>/review_no_maps.txt). Each texture's line in
materials.txt and materials.json then says it has them (pbr 1).

    python -I maps_only.py <extract folder> <pack folder>
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate  # noqa: E402
import pbr  # noqa: E402


def main():
    src, pack = sys.argv[1], sys.argv[2]
    ids = [l.strip() for l in open(os.path.join(pack, "review_no_maps.txt")) if l.strip()]
    index = json.load(open(os.path.join(src, "index.json")))
    classes = json.load(open(os.path.join(src, "classes.json")))
    mats_path = os.path.join(pack, "materials.json")
    mats = json.load(open(mats_path))
    work = os.path.join(pack, "work_maps")
    os.makedirs(work, exist_ok=True)
    models = pbr.Models()
    made = []
    for n, rid in enumerate(ids):
        if rid not in mats or rid not in index:
            continue
        try:
            ok = generate.process(models, src, rid, index[rid], classes[rid]["class"], work)
        except Exception as e:
            print("skip", rid, e)
            continue
        os.remove(os.path.join(work, rid + ".c.png"))  # (its colour: the pack's own)
        if ok:
            made.append(rid)
        if n % 20 == 0:
            print("%d/%d" % (n + 1, len(ids)), flush=True)
    tex = os.path.join(pack, "textures")
    generate.texconv([os.path.join(work, r + ".n.png") for r in made], "BC5_UNORM", tex)
    generate.texconv([os.path.join(work, r + ".m.png") for r in made], "BC7_UNORM", tex)
    made = [r for r in made if os.path.exists(os.path.join(tex, r + ".n.dds")) and os.path.exists(os.path.join(tex, r + ".m.dds"))]
    for rid in made:
        mats[rid]["pbr"] = True
    json.dump(mats, open(mats_path, "w"), indent=1, sort_keys=True)
    txt = os.path.join(pack, "materials.txt")
    out = []
    for line in open(txt).read().splitlines():
        rid = line.split(" ", 1)[0]
        if rid in made:
            m = mats[rid]
            line = "%s %s %d %.3f %.4f %.3f %d %d" % (rid, m["cls"], 1, m["normal"], m["pom"], m["spec"], m["detail"], m["tiles"])
        out.append(line)
    open(txt, "w").write("\n".join(out) + "\n")
    print(len(made), "of", len(ids), "with their maps")


if __name__ == "__main__":
    main()
