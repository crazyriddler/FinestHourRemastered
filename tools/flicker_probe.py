"""Flicker measured: a level started (movies skipped), the player held at a place by noclip and the camera still there,
a burst of frames taken for each case (everything on, then each effect given off alone), and each pixel's change from
frame to frame (its standard deviation over the burst, in 0..255). A still view whose pixels still change is
flickering (a surface's highlight shimmering with the anti-aliasing's jitter, a texture's mips fighting). The cases'
maps are kept as images (work/shots/<tag>_<case>_flicker.png, brighter where it flickers) beside the last frame.

    python tools/flicker_probe.py <level> [--pos "x y z" --yaw Y --pitch P | (the player's own eye)]
        [--cases materials,taa] [--frames 16] [--tag flicker] [--at 40]
"""
import argparse
import os
import re
import subprocess
import sys
import time

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive  # noqa: E402
from level_review import EXE_DIR, START, WORK, read_log  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("level", type=int)
    ap.add_argument("--pos", default="")
    ap.add_argument("--yaw", type=float, default=None)
    ap.add_argument("--pitch", type=float, default=0)
    ap.add_argument("--cases", default="materials,taa")
    ap.add_argument("--frames", type=int, default=16)
    ap.add_argument("--tag", default="flicker")
    ap.add_argument("--at", type=float, default=40)
    ap.add_argument("--keep", action="store_true", help="the frames kept (work/shots/<tag>_<case>_frames.npy)")
    ap.add_argument("--off", default="film_grain", help="effects off through the whole run (film grain: noise of its own)")
    a = ap.parse_args()
    control = os.path.join(WORK, a.tag + ".control.txt")
    open(control, "w").close()
    replay = os.path.join(WORK, a.tag + ".replay.txt")
    drive.build_replay(START, replay)
    with open(replay, "a") as f:
        f.write("%.1f 0 1 0000 0 0 0 0 0 0 0 0 0 0 0 0\n" % 600000)
    drive.kill_build_exe()
    env = dict(os.environ)
    env.update({"XBE_CONTROL": control, "XBE_INPUT_REPLAY": replay, "XBE_SKIP_MOVIES": "1"})
    subprocess.Popen([os.path.join(EXE_DIR, "CODFH.exe")], cwd=EXE_DIR, env=env)
    time.sleep(3)
    t0 = time.time()
    while time.time() - t0 < 600 and "d3d12: shader cache:" not in read_log():
        time.sleep(0.5)
    t0 = time.time() - 1

    def send(line):
        with open(control, "a") as f:
            f.write(line + "\n")

    while time.time() - t0 < 10:
        time.sleep(0.05)
    send("cmd level %d" % a.level)
    while time.time() - t0 < a.at:
        time.sleep(0.05)
    send("cmd noclip on")
    send("cmd hud 0")  # (the compass's star pulses)
    for c in a.off.split(","):
        if c:
            send("cmd fx %s 0" % c)
    time.sleep(1.0)
    if a.pos:
        x, y, z = map(float, a.pos.split())
        send("cmd freecam at %.1f %.1f %.1f %.2f %.2f" % (x, y, z, a.yaw or 0, a.pitch))
    elif a.yaw is not None:
        send("cmd whereami")
        time.sleep(0.8)
        found = re.findall(r"whereami: (\S+) (\S+) (\S+) yaw", read_log())
        if found:
            x, y, z = map(float, found[-1])
            send("cmd freecam at %.1f %.1f %.1f %.2f %.2f" % (x, y, z, a.yaw, a.pitch))
    time.sleep(3)  # (streamed in, the temporal history settled)
    os.makedirs(os.path.join(WORK, "shots"), exist_ok=True)
    results = []
    for n, case in enumerate(["all"] + [c for c in a.cases.split(",") if c]):
        if case != "all":
            send("cmd fx %s 0" % case)
        time.sleep(1.5)
        paths = []
        for i in range(a.frames):
            bmp = os.path.join(WORK, "shots", "%s_%s%d_%02d.bmp" % (a.tag, case, n, i)).replace("\\", "/")
            if os.path.exists(bmp):
                os.remove(bmp)
            send("shot " + bmp)
            paths.append(bmp)
            time.sleep(0.07)
        time.sleep(1.0)
        if case != "all":
            send("cmd fx %s 1" % case)
        results.append(("%s#%d" % (case, n) if case == "all" else case, paths))
    time.sleep(0.5)
    drive.kill_build_exe()
    for case, paths in results:
        frames = []
        for p in paths:
            try:
                frames.append(np.asarray(Image.open(p).convert("L"), np.float32))
                os.remove(p)
            except OSError:
                pass
        if len(frames) < 3:
            print("%s: too few frames" % case)
            continue
        st = np.stack(frames).std(0)
        case = case.replace("#", "")
        if a.keep:
            np.save(os.path.join(WORK, "shots", "%s_%s_frames.npy" % (a.tag, case)), np.stack(frames).astype(np.uint8))
        out = os.path.join(WORK, "shots", "%s_%s_flicker.png" % (a.tag, case))
        Image.fromarray(np.clip(st * 8, 0, 255).astype(np.uint8)).save(out)
        Image.fromarray(frames[-1].astype(np.uint8)).save(os.path.join(WORK, "shots", "%s_%s_frame.png" % (a.tag, case)))
        print("%-12s mean %.2f  p99 %.1f  pixels over 6: %.2f%%  (%d frames)" % (
            case, st.mean(), np.percentile(st, 99), 100 * (st > 6).mean(), len(frames)))


if __name__ == "__main__":
    main()
