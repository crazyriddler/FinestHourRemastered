"""Every level at two moments in widescreen and in 4:3, side by side (the 4:3 middle of the wide frame against the 4:3
frame), to find models out of place on a wide screen: work/sweep_wide/lvNN.png.

    python tools/sweep_wide.py [levels...]
"""
import os
import shutil
import subprocess
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOML = os.path.join(ROOT, "out", "build", "release", "codfh.toml")
BAK = os.path.join(ROOT, "work", "codfh.toml.bak")
LEVELS = [1, 2, 4, 5, 7, 8, 11, 12, 14, 15, 17, 18, 24, 25, 26, 30, 36, 37, 38]
TIMES = [85, 105]


def run(level, tag, wide):
    shutil.copy(BAK, TOML)
    if not wide:
        with open(TOML, "a") as f:
            f.write("widescreen = false\n")
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "drive.py"), "--press", "5:START 8:A 12:A 15:A 18:A",
                    "--cmd", "10:level %d" % level, "--shots", ",".join(str(t) for t in TIMES), "--tag", tag,
                    "--until", str(TIMES[-1] + 1), "--full"], capture_output=True)


def main():
    levels = [int(a) for a in sys.argv[1:]] or LEVELS
    out = os.path.join(ROOT, "work", "sweep_wide")
    os.makedirs(out, exist_ok=True)
    shots = os.path.join(ROOT, "work", "shots")
    for level in levels:
        run(level, "sw%02d" % level, True)
        run(level, "s4%02d" % level, False)
        sheet = Image.new("RGB", (1440, 540 * len(TIMES)))
        for i, t in enumerate(TIMES):
            try:
                a = Image.open(os.path.join(shots, "sw%02d_%d.png" % (level, t))).crop((240, 0, 1680, 1080)).resize((720, 540))
                b = Image.open(os.path.join(shots, "s4%02d_%d.png" % (level, t))).resize((720, 540))
                sheet.paste(a, (0, i * 540))
                sheet.paste(b, (720, i * 540))
            except OSError:
                pass
        sheet.save(os.path.join(out, "lv%02d.png" % level))
        print("level", level, flush=True)
    shutil.copy(BAK, TOML)


if __name__ == "__main__":
    main()
