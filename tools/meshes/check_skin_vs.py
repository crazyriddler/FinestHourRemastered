"""Two sets of remastered skinned meshes checked against each other (check_skin.py's measure, posed by each dump's
bones): the meshes where the new set lands further from the original than the old one did, worst first.

    python tools/meshes/check_skin_vs.py <old pack meshes> [--new work/remaster/pack/meshes] [--worst 20]
"""
import argparse
import glob
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fhm  # noqa: E402
from check_skin import posed  # noqa: E402


def far(src, m):
    a0, b0 = posed(src, src.constants), posed(m, src.constants)
    size = float(np.linalg.norm(a0.max(0) - a0.min(0))) or 1.0
    d = np.concatenate([np.linalg.norm(b0[i:i + 2048, None, :] - a0[None, :, :], axis=2).min(1)
                        for i in range(0, len(b0), 2048)])
    return float(d.max()) / size


def main():
    ap = argparse.ArgumentParser()
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("old")
    ap.add_argument("--new", default=os.path.join(root, "work", "remaster", "pack", "meshes"))
    ap.add_argument("--dump", default=os.path.join(root, "work", "meshes", "all"))
    ap.add_argument("--worst", type=int, default=20)
    a = ap.parse_args()
    rows = []
    for path in glob.glob(os.path.join(a.new, "*.fhm")):
        key = os.path.basename(path)
        src_path, old_path = os.path.join(a.dump, key), os.path.join(a.old, key)
        if not os.path.exists(src_path) or not os.path.exists(old_path):
            continue
        src = fhm.read(src_path)
        if src.constants is None or 1 not in src.reg_list() or 2 not in src.reg_list():
            continue
        if np.abs(src.reg(2)[:, :3] - np.round(src.reg(2)[:, :3])).max() > 1e-4:
            continue
        rows.append((far(src, fhm.read(path)) - far(src, fhm.read(old_path)), key))
    rows.sort(reverse=True)
    for d, key in rows[:a.worst]:
        print(f"{key}: {d:+.3f} of its size further than before")
    print(f"{len(rows)} skinned meshes; {sum(1 for d, _ in rows if d > 0.02)} further than before by 0.02 of their size")


if __name__ == "__main__":
    main()
