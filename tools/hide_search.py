"""Which texture draws something: level runs with a set of textures hidden (hidetex), a screenshot at a moment each.

    python tools/hide_search.py <level> <time> <tag> <id file> <from> <to>    (lines [from, to) of the id file hidden)
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    level, t, tag, ids, a, b = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5]), int(sys.argv[6])
    hide = [l.strip() for l in open(ids) if l.strip()][a:b]
    args = [sys.executable, os.path.join(ROOT, "tools", "drive.py"), "--env", "XBE_SKIP_MOVIES=1", "--press",
            "5:START 8:A 12:A 15:A 18:A", "--cmd", "10:level %s" % level]
    for h in hide:
        args += ["--cmd", "9:hidetex " + h]
    args += ["--shots", t, "--until", str(float(t) + 1), "--full", "--tag", tag]
    subprocess.run(args, capture_output=True)
    print(tag, len(hide))


if __name__ == "__main__":
    main()
