"""The GPU's time per draw ("cmd drawtimes": every draw of one frame timed alone, logged as "drawtime N: ... us tex ..
vs .. ps .."), summed by pixel shader and by texture: what the scene's draws spend their time on.

    python tools/drawtimes_top.py <log> [--top 20]

Each draw timed alone runs with the GPU idle around it: the times are each draw's own, a little over what it costs
among the others; their order is what counts.
"""
import argparse
import collections
import re


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--top", type=int, default=20)
    a = ap.parse_args()
    pat = re.compile(r"drawtime (\d+): ([\d.]+) us tex (\w+) vs (\w+) ps (\w+) rt (\d+)x(\d+) count (\d+) topo (\w+) blend (\d+) scene (\d+)")
    frames = []
    cur = []
    for line in open(a.log, encoding="utf-8", errors="replace"):
        m = pat.search(line)
        if m:
            i = int(m.group(1))
            if i == 0 and cur:
                frames.append(cur)
                cur = []
            cur.append(m.groups())
        elif "drawtimes:" in line and cur:
            frames.append(cur)
            cur = []
    if cur:
        frames.append(cur)
    for n, f in enumerate(frames):
        total = sum(float(d[1]) for d in f)
        print("frame %d: %d draws, %.3f ms" % (n, len(f), total / 1000))
        for key, name in ((4, "pixel shader"), (2, "texture"), (3, "vertex shader")):
            sums = collections.defaultdict(lambda: [0.0, 0, 0])
            for d in f:
                s = sums[d[key]]
                s[0] += float(d[1])
                s[1] += 1
                s[2] += int(d[7])
            print("  by %s:" % name)
            for k, (us, cnt, verts) in sorted(sums.items(), key=lambda kv: -kv[1][0])[: a.top]:
                print("    %s  %8.1f us  %4d draws  %7d vertices  %4.1f%%" % (k, us, cnt, verts, 100 * us / max(total, 1e-9)))


if __name__ == "__main__":
    main()
