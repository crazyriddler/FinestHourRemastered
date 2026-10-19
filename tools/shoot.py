"""Shots for the trailer and the stills: a level started (movies skipped), the player made immune, the free camera
moved along a path (or held) while the game records the screen (control "record", NVENC), with a set of effects on;
a shot can be filmed twice, the second time with the remaster and every effect off (the original's look), for the
before/after comparisons. Every frame's place is the free camera's: the two takes match.

    python tools/shoot.py <shots.json> [names...] [--out work/trailer/clips]

shots.json: a list of {"name", "level", "checkpoint" (optional "N s1 s2 s3 s4"), "at" (seconds to the level's play),
"from": [x, y, z, yaw, pitch], "to": [...] (optional: held at "from"), "seconds", "settle" (seconds before recording),
"compare": true (a second take with everything off), "fx": {"name": 0/1} (changes for this shot), "cmds": [...]
(console commands before recording, e.g. "god on"), "hud": false (the HUD hidden: the free camera has none),
"still": true (also a still at the middle of each take), "follow": [up, pitch, yaw] (above the player's eye and
following it, in place of "from"), "view": true (the player's own view), "look": [dx, dy, steps] (the view turned by
mouse looks through the take), "hud": true (the HUD kept)}

Clips: <out>/<name>.mp4 (and <name>_off.mp4), stills <out>/<name>.png (and _off).
"""
import argparse
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive  # noqa: E402
from level_review import EXE_DIR, START, WORK, read_log  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FFMPEG = os.path.join(os.path.dirname(ROOT), "tools", "ffmpeg", "ffmpeg-master-latest-win64-gpl", "bin", "ffmpeg.exe")

# every effect and the remaster: off for the original's look (game_info.cpp's fx names)
EFFECTS = ["bloom", "hit_reactions", "ao", "contact_shadows", "dynamic_lights", "light_halos", "soft_particles",
           "heat_haze", "sun_shafts", "aim_dof", "explosion_flash", "flash_shadows", "auto_exposure", "cloud_shadows",
           "volumetric_fog", "soft_shadows", "bounce_light", "character_light", "lit_smoke", "ambient_particles",
           "wind", "barrel_heat", "animated_fire", "shockwaves", "water", "water_interaction", "snow_tracks",
           "lens_flare", "sharp_distance", "weapon_materials", "character_detail", "color_grade", "film_grain",
           "smooth_models", "reflections", "sun_moon", "clouds", "materials", "parallax", "detail", "pixel_lighting",
           "height_blend", "aa", "motion_blur", "remaster", "shadows"]


def film(shot, out, take_off):
    control = os.path.join(WORK, "shoot.control.txt")
    open(control, "w").close()
    replay = os.path.join(WORK, "shoot.replay.txt")
    drive.build_replay(START, replay)
    with open(replay, "a") as f:
        f.write("%.1f 0 1 0000 0 0 0 0 0 0 0 0 0 0 0 0\n" % 600000)
    drive.kill_build_exe()
    env = dict(os.environ)
    env.update({"XBE_CONTROL": control, "XBE_INPUT_REPLAY": replay, "XBE_SKIP_MOVIES": "1", "XBE_FFMPEG": FFMPEG})
    subprocess.Popen([os.path.join(EXE_DIR, "CODFH.exe")], cwd=EXE_DIR, env=env)
    t0 = time.time()
    time.sleep(3)
    while time.time() - t0 < 600 and "d3d12: shader cache:" not in read_log():
        time.sleep(0.5)
    t0 = time.time() - 1

    def send(line):
        with open(control, "a") as f:
            f.write(line + "\n")

    def wait_until(s):
        while time.time() - t0 < s:
            time.sleep(0.05)

    wait_until(10)
    send("cmd level %d" % shot["level"])
    at = shot.get("at", 34)
    if shot.get("checkpoint"):
        wait_until(58)
        send("cmd checkpoint " + shot["checkpoint"])
        time.sleep(0.5)
        for name, wait in (("START", 1.5), ("DOWN", 1.0), ("A", 1.5), ("DOWN", 1.0), ("A", 0)):
            send("pad %s 150" % name)
            time.sleep(wait)
        at = max(at, 75)
    wait_until(at)
    for _ in range(3):
        send("cmd god on")
        time.sleep(0.3)
    for name, on in shot.get("fx", {}).items():
        send("cmd fx %s %d" % (name, on))
    if take_off:
        for name in EFFECTS:
            send("cmd fx %s 0" % name)
    if not shot.get("hud"):
        send("cmd hud 0")
    for c in shot.get("cmds", []):
        send("cmd " + c)
    follow = shot.get("follow")  # [up, pitch, yaw]: above the player's eye, turned from its view (follows it)
    view = shot.get("view")  # the player's own view: the game's camera
    a = shot.get("from")
    b = shot.get("to", a)
    if follow:
        send("cmd freecam %.1f %.1f %.1f" % tuple(follow))
    elif not view:
        send("cmd freecam at %.1f %.1f %.1f %.2f %.2f" % tuple(a))
    time.sleep(shot.get("settle", 2.5))
    tag = shot["name"] + ("_off" if take_off else "")
    clip = os.path.join(out, tag + ".mp4").replace("\\", "/")
    if os.path.exists(clip):
        os.remove(clip)
    seconds = shot.get("seconds", 6)
    send("record " + clip)
    time.sleep(0.4)
    if a and not follow and not view:
        send("cmd freecam path %s %s %.2f" % (" ".join("%.2f" % v for v in a), " ".join("%.2f" % v for v in b), seconds))
    # control lines at times in the take: [[seconds, "pad RT 300"], [seconds, "cmd hurt"], ...]
    if shot.get("during"):
        import threading

        def during():
            t0d = time.time()
            for at, line in shot["during"]:
                while time.time() - t0d < at:
                    time.sleep(0.02)
                send(line)
        threading.Thread(target=during, daemon=True).start()
    # mouse looks spread over the take (the player's own view turning): [dx per step, dy per step, steps]
    if shot.get("look"):
        dx, dy, steps = shot["look"]
        import threading

        def turn():
            for _ in range(int(steps)):
                send("cmd look %d %d" % (dx, dy))
                time.sleep(seconds / steps)
        threading.Thread(target=turn, daemon=True).start()
    if shot.get("still"):
        time.sleep(seconds / 2)
        bmp = os.path.join(out, tag + ".bmp").replace("\\", "/")
        send("shot " + bmp)
        time.sleep(seconds / 2 + 0.6)
    else:
        time.sleep(seconds + 0.6)
    send("record stop")
    time.sleep(2.0)
    drive.kill_build_exe()
    time.sleep(1.0)
    if shot.get("still"):
        from PIL import Image
        bmp = os.path.join(out, tag + ".bmp")
        if os.path.exists(bmp):
            Image.open(bmp).convert("RGB").save(os.path.join(out, tag + ".png"))
            os.remove(bmp)
    print(tag, "ok" if os.path.exists(clip) and os.path.getsize(clip) > 10000 else "MISSING", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("shots")
    ap.add_argument("names", nargs="*")
    ap.add_argument("--out", default=os.path.join(WORK, "trailer", "clips"))
    ap.add_argument("--only-off", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    shots = json.load(open(a.shots))
    for shot in shots:
        if a.names and shot["name"] not in a.names:
            continue
        if not a.only_off:
            film(shot, a.out, False)
        if shot.get("compare"):
            film(shot, a.out, True)


if __name__ == "__main__":
    main()
