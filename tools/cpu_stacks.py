"""The sampling profiler's stacks (XBE_PERF_SAMPLE_STACKS: "perf [label]: stack: Nx thread T name: f < g < ...") from
vk_bench runs, summed by function: for each view and thread, the share of its busy samples each function is on the
stack for (inclusive), the functions named by a pattern or the most taken.

    python tools/cpu_stacks.py <tag> [--api d3d12] [--thread render] [--top 30] [--match WeaponPieces,CollectEmitter]
"""
import argparse
import collections
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def name_of(frame):
    frame = frame.strip()
    frame = re.sub(r"^\[.*?\]\s*", "", frame)  # (a system frame's module, innermost)
    frame = re.sub(r" @\S+$", "", frame)
    frame = frame.replace("`anonymous namespace'::", "")
    return frame[:90]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tag")
    ap.add_argument("--api", default="d3d12")
    ap.add_argument("--thread", default="render")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--match", default="")
    a = ap.parse_args()
    path = os.path.join(ROOT, "work", "vk", a.tag, "raw_%s.txt" % a.api)
    views = collections.OrderedDict()
    pat = re.compile(r"perf \[(.*?)\]: stack:\s*(\d+)x thread \d+ ([^:]*): (.*)$")
    for line in open(path, encoding="utf-8", errors="replace"):
        m = pat.search(line.rstrip())
        if not m or a.thread not in m.group(3):
            continue
        view, n, frames = m.group(1), int(m.group(2)), m.group(4).split(" < ")
        v = views.setdefault(view, [0, collections.Counter()])
        v[0] += n
        for f in set(name_of(f) for f in frames):
            v[1][f] += n
    match = [s for s in a.match.split(",") if s]
    for view, (total, c) in views.items():
        print("%s: %d samples on the stacks listed" % (view, total))
        rows = [(f, n) for f, n in c.items() if any(s in f for s in match)] if match else c.most_common(a.top)
        for f, n in sorted(rows, key=lambda r: -r[1])[: a.top]:
            print("  %5.1f%%  %s" % (100.0 * n / total, f))


if __name__ == "__main__":
    main()
