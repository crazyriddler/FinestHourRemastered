"""Brings a pack up to date while or after generate.py runs: compresses the work maps not compressed yet and writes
materials.txt from materials.json (only textures whose colour map is there).

    python -I pack.py <pack folder>
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate  # noqa: E402


def main():
    pack = sys.argv[1]
    work = os.path.join(pack, "work")
    tex = os.path.join(pack, "textures")
    os.makedirs(tex, exist_ok=True)

    def pending(suffix):
        return [os.path.join(work, f) for f in sorted(os.listdir(work)) if f.endswith(suffix) and
                not os.path.exists(os.path.join(tex, f[:-4] + ".dds"))]

    generate.texconv(pending(".c.png"), "BC7_UNORM", tex)
    generate.texconv(pending(".n.png"), "BC5_UNORM", tex)
    generate.texconv(pending(".m.png"), "BC7_UNORM", tex)
    mats = json.load(open(os.path.join(pack, "materials.json")))
    ready = {k: v for k, v in mats.items() if os.path.exists(os.path.join(tex, k + ".c.dds"))
             and (not v["pbr"] or (os.path.exists(os.path.join(tex, k + ".n.dds"))
                                   and os.path.exists(os.path.join(tex, k + ".m.dds"))))}
    generate.write_index(ready, pack)
    print(len(ready), "of", len(mats), "textures packed")


if __name__ == "__main__":
    main()
