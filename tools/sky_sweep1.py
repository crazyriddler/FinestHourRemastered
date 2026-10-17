"""Level 1 (the Volga crossing): the camera tilted up into the smoke over Stalingrad, then turned step by step, a
screenshot at each step. Effects given are off from the start (fx), so runs can be compared.

    python tools/sky_sweep1.py <tag> [effect ...] [--env NAME=VALUE ...]
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    args = sys.argv[1:]
    tag = args.pop(0)
    envs, effects = [], []
    while args:
        a = args.pop(0)
        if a == "--env":
            envs.append(args.pop(0))
        else:
            effects.append(a)
    press = ["5:START", "8:A", "12:A", "15:A", "18:A", "30:RY+/0.35"]
    shots = []
    t = 31.5
    for _ in range(int(os.environ.get("SWEEP_N", "10"))):
        shots.append("%.1f" % t)
        press.append("%.2f:RX+/0.25" % (t + 0.3))
        t += 1.2
    cmd = [sys.executable, os.path.join(ROOT, "tools", "drive.py"), "--env", "XBE_SKIP_MOVIES=1", "--press", " ".join(press),
           "--cmd", "10:level 1"]
    for e in envs:
        cmd += ["--env", e]
    for e in effects:
        cmd += ["--cmd", "11:fx %s 0" % e]
    cmd += ["--shots", ",".join(shots), "--until", "%.1f" % (t + 0.5), "--full", "--tag", "ss_" + tag]
    subprocess.run(cmd, capture_output=True)
    print(" ".join("ss_%s_%g" % (tag, float(s)) for s in shots))


if __name__ == "__main__":
    main()
