"""The GPU's passes from vk_bench runs side by side: each run's "perf [...]: GPU average" lines (work/vk/<tag>/raw_<api>.txt),
their passes' milliseconds averaged over the views measured, two tags compared (the change per pass, the total).

    python tools/gpu_passes.py <before tag> <after tag> [--api d3d12]
"""
import argparse
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(ROOT, "work", "vk")


def load(tag, api):
    path = os.path.join(WORK, tag, "raw_%s.txt" % api)
    runs = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.search(r"perf \[(.*?)\]: GPU average: ([\d.]+) ms:(.*)$", line.rstrip())
        if not m:
            continue
        passes = {}
        # (a pass's label may hold commas: "2d, hud, output 0.03")
        for name, ms in re.findall(r"\s*(.+?) (\d+\.\d+)(?:,|$)", m.group(3)):
            passes[name] = passes.get(name, 0) + float(ms)
        runs[m.group(1)] = (float(m.group(2)), passes)
    return runs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("--api", default="d3d12")
    ap.add_argument("--top", type=int, default=16)
    a = ap.parse_args()
    b, c = load(a.before, a.api), load(a.after, a.api)
    keys = sorted(set(b) & set(c))
    if not keys:
        print("no views in common")
        return
    tb = sum(b[k][0] for k in keys) / len(keys)
    tc = sum(c[k][0] for k in keys) / len(keys)
    names = set()
    for k in keys:
        names |= set(b[k][1]) | set(c[k][1])
    rows = []
    for n in names:
        mb = sum(b[k][1].get(n, 0) for k in keys) / len(keys)
        mc = sum(c[k][1].get(n, 0) for k in keys) / len(keys)
        rows.append((n, mb, mc))
    rows.sort(key=lambda r: -max(r[1], r[2]))
    print("%s, %d views: total %.3f -> %.3f ms (%+.3f)" % (a.api, len(keys), tb, tc, tc - tb))
    for n, mb, mc in rows[: a.top]:
        print("  %-22s %.3f -> %.3f (%+.3f)" % (n, mb, mc, mc - mb))


if __name__ == "__main__":
    main()
