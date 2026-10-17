"""The start of each level with the player firing: which glowing textures lit the scene (dynamic lights), any error,
and a sheet of screenshots: work/lights/lvNN.png and work/lights/summary.txt. Movies are skipped.

    python tools/lights_sweep.py [levels...]
"""
import os
import re
import subprocess
import sys
from collections import Counter

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "out", "build", "release", "logs", "codfh.log")
SHOTS = os.path.join(ROOT, "work", "shots")
LEVELS = [1, 2, 4, 5, 7, 8, 11, 12, 14, 15, 17, 18]
TIMES = [24.5, 27, 30, 34]


def main():
    levels = [int(a) for a in sys.argv[1:]] or LEVELS
    out = os.path.join(ROOT, "work", "lights")
    os.makedirs(out, exist_ok=True)
    summary = []
    for level in levels:
        tag = "li%02d" % level
        press = "5:START 8:A 12:A 15:A 18:A 24:RT/1.2 26.5:RT/1.2 29.5:RT/1.5 33.5:RT/1"
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "drive.py"), "--press", press, "--cmd",
                        "10:level %d" % level, "--shots", ",".join(str(t) for t in TIMES), "--tag", tag, "--until",
                        str(TIMES[-1] + 1), "--env", "XBE_SKIP_MOVIES=1", "--env", "XBE_EMIT_LOG=1"], capture_output=True)
        log = open(LOG, encoding="utf-8", errors="replace").read()
        kinds = Counter(re.findall(r"emit: light ([0-9a-f]{16})", log))
        errors = [l for l in log.splitlines() if "error" in l.lower()][:3]
        line = "level %2d: lights %s%s" % (level, dict(kinds) or "none", ("  ERRORS: " + " | ".join(errors)) if errors else "")
        print(line, flush=True)
        summary.append(line)
        sheet = Image.new("RGB", (960 * 2, 540 * 2))
        for i, t in enumerate(TIMES):
            try:
                im = Image.open(os.path.join(SHOTS, "%s_%g.png" % (tag, t))).resize((960, 540))
                sheet.paste(im, ((i % 2) * 960, (i // 2) * 540))
            except OSError:
                pass
        sheet.save(os.path.join(out, "lv%02d.png" % level))
    with open(os.path.join(out, "summary.txt"), "w") as f:
        f.write("\n".join(summary) + "\n")


if __name__ == "__main__":
    main()
