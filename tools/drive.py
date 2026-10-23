"""Drive the game for tests: scripted pad presses (XBE_INPUT_REPLAY) and window
screenshots at given times.

    py -3 tools/drive.py --press "8:START 11:A 13:DOWN 14:A" --shots 10,15,20 --tag menu
    py -3 tools/drive.py --replay inputs.txt --shots 60,90 --tag play --until 100

Times are seconds from the game's start. Buttons: A B X Y BLACK WHITE LT RT
(analog), UP DOWN LEFT RIGHT START BACK LS RS (digital), and LX+/LX-/LY+/LY-
for a full stick push. "8:A/0.5" holds A for 0.5 s (default 0.15 s).
Screenshots go to work/shots/<tag>_<s>.png, taken by the game itself (XBE_CONTROL): the
monitor, the focus and other windows do not matter. --cmd queues game console commands.
"""
import argparse
import ctypes
import ctypes.wintypes as w
import os
import subprocess
import sys
import time

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# (XBE_TOOL_EXE_DIR: a copy of the game of its own to run, tools/instances.py, for runs side by side)
EXE_DIR = os.environ.get("XBE_TOOL_EXE_DIR") or os.path.join(ROOT, "out", "build", "release")
WORK = os.path.join(ROOT, "work")

DIGITAL = {"UP": 0x1, "DOWN": 0x2, "LEFT": 0x4, "RIGHT": 0x8, "START": 0x10, "BACK": 0x20, "LS": 0x40, "RS": 0x80}
ANALOG = {"A": 0, "B": 1, "X": 2, "Y": 3, "BLACK": 4, "WHITE": 5, "LT": 6, "RT": 7}
STICK = {"LX+": (0, 32767), "LX-": (0, -32768), "LY+": (1, 32767), "LY-": (1, -32768),
         "RX+": (2, 32767), "RX-": (2, -32768), "RY+": (3, 32767), "RY-": (3, -32768)}


def build_replay(spec, path):
    events = []  # (ms, buttons, analog, sticks)
    for item in spec.split():
        t, _, rest = item.partition(":")
        name, _, dur = rest.partition("/")
        t = float(t)
        dur = float(dur) if dur else 0.15
        events.append((t, name.upper(), True))
        events.append((t + dur, name.upper(), False))
    events.sort()
    state_b, state_a, state_s = 0, [0] * 8, [0] * 4
    lines = ["0.0 0 1 0000 0 0 0 0 0 0 0 0 0 0 0 0"]
    for t, name, down in events:
        if name in DIGITAL:
            state_b = (state_b | DIGITAL[name]) if down else (state_b & ~DIGITAL[name])
        elif name in ANALOG:
            state_a[ANALOG[name]] = 255 if down else 0
        elif name in STICK:
            axis, v = STICK[name]
            state_s[axis] = v if down else 0
        elif "=" in name and name.split("=")[0] in ("LX", "LY", "RX", "RY"):  # RX=0.3: part way
            axis = ("LX", "LY", "RX", "RY").index(name.split("=")[0])
            state_s[axis] = int(max(-1, min(1, float(name.split("=")[1]))) * 32767) if down else 0
        else:
            sys.exit("unknown button " + name)
        lines.append("%.1f 0 1 %04X %s %d %d %d %d" % (t * 1000, state_b, " ".join(map(str, state_a)), *state_s))
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def game_pids():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq CODFH.exe", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    return {int(l.split(",")[1].strip('"')) for l in out.splitlines() if l.startswith('"CODFH.exe"')}


def find_window(_pid=None):
    """The visible CODFH.exe window; a relaunch moves it to a new process."""
    user32 = ctypes.windll.user32
    pids = game_pids()
    found = []

    @ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    def cb(hwnd, _):
        p = w.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value in pids and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(cb, 0)
    return found[-1] if found else None


def shot(pid, path):
    hwnd = find_window(pid)
    if not hwnd:
        return False
    user32 = ctypes.windll.user32
    r = w.RECT()
    pt = w.POINT(0, 0)
    user32.GetClientRect(hwnd, ctypes.byref(r))
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    user32.SetForegroundWindow(hwnd)
    img = ImageGrab.grab(bbox=(pt.x, pt.y, pt.x + r.right, pt.y + r.bottom), all_screens=True)
    img.thumbnail((960, 960))
    img.save(path)
    return True


def kill_build_exe():
    """the build's CODFH.exe only: a copy the player runs elsewhere is left alone"""
    exe = os.path.normcase(os.path.abspath(os.path.join(EXE_DIR, "CODFH.exe")))
    ps = ("Get-CimInstance Win32_Process -Filter \"Name='CODFH.exe'\" | "
          "Where-Object { $_.ExecutablePath -and ([IO.Path]::GetFullPath($_.ExecutablePath)).ToLower() -eq '%s' } | "
          "ForEach-Object { Stop-Process -Id $_.ProcessId -Force }" % exe.lower().replace("'", "''"))
    subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--press", default="")
    ap.add_argument("--replay", default="")
    ap.add_argument("--shots", default="")
    ap.add_argument("--cmd", action="append", default=[], help='"<s>:<console command>", e.g. "12:open x02_prison_a"')
    ap.add_argument("--line", action="append", default=[], help='"<s>:<control line>", e.g. "40:threads"')
    ap.add_argument("--tag", default="drive")
    ap.add_argument("--until", type=float, default=0)
    ap.add_argument("--keep", action="store_true", help="leave the game running")
    ap.add_argument("--full", action="store_true", help="screenshots at full size")
    ap.add_argument("--env", action="append", default=[], help="NAME=VALUE for the game")
    a = ap.parse_args()
    os.makedirs(os.path.join(WORK, "shots"), exist_ok=True)
    replay = a.replay
    if a.press:
        replay = os.path.join(WORK, a.tag + ".replay.txt")
        build_replay(a.press, replay)
        # the pad stays connected (and idle) to the end of the run
        with open(replay, "a") as f:
            f.write("%.1f 0 1 0000 0 0 0 0 0 0 0 0 0 0 0 0\n" % ((max(a.until, 1) + 5) * 1000))
    kill_build_exe()
    # the game takes screenshots and console commands from a control file (src/core/control.h)
    control = os.path.join(WORK, a.tag + ".control.txt")
    open(control, "w").close()
    env = dict(os.environ)
    env["XBE_CONTROL"] = control
    if replay:
        env["XBE_INPUT_REPLAY"] = os.path.abspath(replay)
    for kv in a.env:
        k, _, v = kv.partition("=")
        env[k] = v
    subprocess.Popen([os.path.join(EXE_DIR, "CODFH.exe")], cwd=EXE_DIR, env=env)
    t0 = time.time()
    events = [(float(s), "shot") for s in a.shots.split(",") if s.strip()]
    for c in a.cmd:
        t, _, text = c.partition(":")
        events.append((float(t), "cmd " + text))
    for c in a.line:
        t, _, text = c.partition(":")
        events.append((float(t), text))
    events.sort()
    end = max([a.until] + [t for t, _ in events]) + 1.5
    bmps = []
    for t, what in events:
        while time.time() - t0 < t:
            time.sleep(0.05)
        if what == "shot":
            bmp = os.path.join(WORK, "shots", "%s_%g.bmp" % (a.tag, t))
            if os.path.exists(bmp):
                os.remove(bmp)
            bmps.append(bmp)
            line = "shot " + bmp
        else:
            line = what
        with open(control, "a") as f:
            f.write(line + "\n")
    while time.time() - t0 < end:
        time.sleep(0.1)
    if not a.keep:
        kill_build_exe()
    for bmp in bmps:
        if os.path.exists(bmp):
            img = Image.open(bmp)
            if not a.full:
                img.thumbnail((960, 960))
            img.save(bmp[:-4] + ".png")
            os.remove(bmp)
            print(bmp[:-4] + ".png")
        else:
            print("no shot " + bmp)


if __name__ == "__main__":
    main()
