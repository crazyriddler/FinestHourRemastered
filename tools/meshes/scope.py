"""The remastered meshes the release ships, by what they draw (their texture's name): the soldiers (bodies, heads,
hands, their gear), the weapons in first and third person, and the pickups (weapons on the ground, medkits,
ammunition, grenades). The scenery and the vehicles wait for a closer review: their meshes are moved out of the pack
to work/meshes/pack_held, and listed in tools/release/retired_files.txt so an update takes them out of an
installation that had them.

    python tools/meshes/scope.py [--dry]
"""
import argparse
import glob
import json
import os
import re
import shutil
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PACK = os.path.join(ROOT, "work", "remaster", "pack", "meshes")
HELD = os.path.join(ROOT, "work", "meshes", "pack_held")
DUMP = os.path.join(ROOT, "work", "meshes", "all")
RETIRED = os.path.join(ROOT, "tools", "release", "retired_files.txt")

SOLDIERS = r"^(AT_|Arm_|BT\d|Bt\d|BM\d|HT|HGS|HM_|GT|DeadRussian|aDeadHead|MoreClones_Injured|face1$|hand$|hand_African|" \
           r"Openface|Backpack|backpack_US|rightfrontpacks|antenapouch|holster|holder|Stiel_Bag|scarf|USknife|gunner$)"
WEAPONS = r"(Back|Front|_back|_front)$|^(BazookaBody|BazookaSite|MosinAmmo|MWT_MG42_|BearNade|grenade$|pistol$|knife$)"
PICKUPS = r"^(PUT|PU_|RoundMedCan|ammopack|DemoCharge|TellerMine)"
VEHICLES = re.compile(r"^(VT_|VM_|VTrack|Russian_car|truck|Volga_body|Jeep|T34_)", re.I)
KEEP = re.compile("|".join((SOLDIERS, WEAPONS, PICKUPS)))


def name_of(key, index):
    with open(os.path.join(DUMP, key + ".fhm"), "rb") as f:
        tex = "%016x" % struct.unpack_from("<Q", f.read(48), 12)[0]
    names = index.get(tex, {}).get("names", [])
    return names[0].strip() if names else ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry", action="store_true")
    a = ap.parse_args()
    index = json.load(open(os.path.join(ROOT, "work", "remaster", "extract", "index.json")))
    os.makedirs(HELD, exist_ok=True)
    kept, held = [], []
    for path in sorted(glob.glob(os.path.join(PACK, "*.fhm"))):
        key = os.path.basename(path)[:-4]
        name = name_of(key, index)
        (kept if KEEP.search(name) and not VEHICLES.search(name) else held).append(key)
    retired = set()
    if os.path.exists(RETIRED):
        retired = {l.strip() for l in open(RETIRED) if l.strip() and not l.startswith("#")}
    retired |= {"remaster/meshes/%s.fhm" % k for k in held}
    print(f"{len(kept)} kept, {len(held)} held back")
    if a.dry:
        return
    for key in held:
        shutil.move(os.path.join(PACK, key + ".fhm"), os.path.join(HELD, key + ".fhm"))
    with open(RETIRED, "w") as f:
        f.write("# files an update takes out of an installation (once shipped, no longer: tools/meshes/scope.py)\n")
        f.writelines(r + "\n" for r in sorted(retired))


if __name__ == "__main__":
    main()
