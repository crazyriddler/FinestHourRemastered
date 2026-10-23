"""Copies of the built game to run side by side (sweeps, checks): out/inst/<n>, each with its own log, shader cache,
settings and saves; the executable a hard link to the build's (no room taken; made again after each build: a build
writes a new file), the remaster pack the build's own junction's target, assets found above as the build's are.
A tool runs in one with XBE_TOOL_EXE_DIR=<its folder> (drive.py, level_review.py).

    python tools/instances.py <count>          (made, or brought up to the build)
    python tools/instances.py --run <count> <command with {exe} {n}> [levels...]
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "out", "build", "release")
INST = os.path.join(ROOT, "out", "inst")


def make(n):
    d = os.path.join(INST, str(n))
    os.makedirs(os.path.join(d, "logs"), exist_ok=True)
    exe = os.path.join(d, "CODFH.exe")
    if os.path.exists(exe):
        os.remove(exe)
    os.link(os.path.join(BUILD, "CODFH.exe"), exe)
    pack = os.path.realpath(os.path.join(BUILD, "remaster"))
    if not os.path.exists(os.path.join(d, "remaster")):
        subprocess.run(["cmd", "/c", "mklink", "/J", os.path.join(d, "remaster"), pack], capture_output=True)
    for sub in ("lang", "hdd", "shadercache", "tools"):
        src, dst = os.path.join(BUILD, sub), os.path.join(d, sub)
        if os.path.isdir(src) and not os.path.exists(dst):
            shutil.copytree(src, dst)
    # windowed at 720p: four side by side fill a 1440p screen, each in its quarter (place_windows)
    toml = open(os.path.join(BUILD, "codfh.toml"), encoding="utf-8").read().splitlines()
    sets = {"display_mode": "'windowed'", "width": "1280", "height": "720"}
    toml = [("%s = %s" % (line.split("=")[0].strip(), sets[line.split("=")[0].strip()])
             if "=" in line and line.split("=")[0].strip() in sets else line) for line in toml]
    open(os.path.join(d, "codfh.toml"), "w", encoding="utf-8").write("\n".join(toml) + "\n")
    return d


def place_windows(dirs):
    """each copy's game window moved to its quarter of the screen (copy 1 top left, 2 top right, 3 and 4 below)"""
    import ctypes
    import ctypes.wintypes as w
    user, kernel = ctypes.windll.user32, ctypes.windll.kernel32
    sw, sh = user.GetSystemMetrics(0), user.GetSystemMetrics(1)
    want = {os.path.normcase(os.path.join(d, "CODFH.exe")): i for i, d in enumerate(dirs)}
    found = []

    def each(hwnd, _):
        if not user.IsWindowVisible(hwnd):
            return True
        pid = w.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        h = kernel.OpenProcess(0x1000, False, pid.value)  # PROCESS_QUERY_LIMITED_INFORMATION
        if h:
            buf = ctypes.create_unicode_buffer(1024)
            size = w.DWORD(1024)
            if kernel.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
                i = want.get(os.path.normcase(buf.value))
                if i is not None:
                    found.append((hwnd, i))
            kernel.CloseHandle(h)
        return True

    proc = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)(each)
    user.EnumWindows(proc, 0)
    for hwnd, i in found:
        x, y = (i % 2) * sw // 2, (i // 2) * sh // 2
        r = w.RECT()
        user.GetWindowRect(hwnd, ctypes.byref(r))
        if (r.left, r.top) != (x, y):
            user.SetWindowPos(hwnd, 0, x, y, 0, 0, 0x0001 | 0x0004 | 0x0010)  # NOSIZE NOZORDER NOACTIVATE


def main():
    if sys.argv[1] == "--run":
        count, cmd, levels = int(sys.argv[2]), sys.argv[3], sys.argv[4:]
        dirs = [make(i + 1) for i in range(count)]
        # the levels dealt out in turn, each copy running its share one after another
        shares = [levels[i::count] for i in range(count)]
        procs = []
        for i, (d, share) in enumerate(zip(dirs, shares)):
            if not share:
                continue
            env = dict(os.environ, XBE_TOOL_EXE_DIR=d)
            line = cmd.format(exe=d, n=i + 1, levels=" ".join(share))
            print("copy %d: %s" % (i + 1, line), flush=True)
            procs.append(subprocess.Popen(line, shell=True, env=env, cwd=ROOT))
        import time
        while any(p.poll() is None for p in procs):
            place_windows(dirs)
            time.sleep(2)
        return
    for i in range(int(sys.argv[1])):
        print(make(i + 1))


if __name__ == "__main__":
    main()
