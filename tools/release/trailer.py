"""The trailer, cut from tools/shoot.py's clips with ffmpeg: title cards, shots with a caption naming the feature,
before/after comparisons (the effects-off take and the on take of the same camera, a line sweeping across them),
the game's own music under it all. Also the README's GIFs and comparison stills.

    python tools/release/trailer.py <edit.json> [--clips work/trailer/clips] [--out work/trailer/trailer.mp4]
    python tools/release/trailer.py --gif <clip.mp4> <start> <seconds> <out.gif> [--width 960]

edit.json: {"music": "work/trailer/music2/s_47.wav", "music_start": 95.0, "segments": [...]} where a segment is
  {"title": "FINEST HOUR", "sub": "REMASTERED", "seconds": 4}                    a title card
  {"clip": "omaha_sun", "start": 0.5, "seconds": 5, "caption": "...", "sub": "..."}    a shot
  {"compare": "omaha_sun", "start": 0.5, "seconds": 6, "caption": "..."}              before/after of a shot
Every segment fades in and out of black over "fade" seconds (0.25 by default, 0 for a hard cut).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FFMPEG = os.path.join(os.path.dirname(ROOT), "tools", "ffmpeg", "ffmpeg-master-latest-win64-gpl", "bin", "ffmpeg.exe")
FONT_BOLD = os.path.join(ROOT, "tools", "installer", "art", "Jost-Bold.ttf")
FONT_MONO = os.path.join(ROOT, "tools", "installer", "art", "CourierPrime-Bold.ttf")
W, H, FPS = 2560, 1440, 60
GOLD = "0xE0B45A"
# intermediate segments: the same encoder settings each, so they join without encoding again
ENC = ["-c:v", "h264_nvenc", "-preset", "p7", "-tune", "hq", "-rc", "vbr", "-cq", "16", "-b:v", "0",
       "-pix_fmt", "yuv420p", "-r", str(FPS)]


def run(args, cwd):
    r = subprocess.run([FFMPEG, "-hide_banner", "-loglevel", "error", "-y"] + args, cwd=cwd)
    if r.returncode:
        sys.exit("ffmpeg failed: " + " ".join(args))


def esc(text):
    return text.replace("\\", "\\\\").replace("'", "’").replace(":", "\\:").replace("%", "\\%")


def text(t, font, size, x, y, color="white", t0=0.4, t1=None, alpha=1.0):
    """drawtext, faded in at t0 (and out before t1)"""
    a = "min(1,max(0,(t-%.2f)/0.35))" % t0
    if t1 is not None:
        a = "min(%s,max(0,(%.2f-t)/0.35))" % (a, t1)
    return ("drawtext=fontfile=%s:text='%s':fontsize=%d:fontcolor=%s:x=%s:y=%s:alpha='%.2f*%s'"
            ":shadowcolor=black@0.6:shadowx=3:shadowy=3" % (font, esc(t), size, color, x, y, alpha, a))


def caption(seg, seconds):
    """the feature's name low on the left, its line under it"""
    out = []
    if seg.get("caption"):
        out.append(text(seg["caption"].upper(), "bold.ttf", 64, "120", "h-250", GOLD, 0.3, seconds - 0.2))
    if seg.get("sub"):
        out.append(text(seg["sub"], "mono.ttf", 34, "124", "h-170", "white", 0.5, seconds - 0.2, 0.9))
    return out


def fades(seconds, fade):
    if fade <= 0:
        return []
    return ["fade=t=in:st=0:d=%.2f" % fade, "fade=t=out:st=%.2f:d=%.2f" % (seconds - fade, fade)]


def sweep(s):
    """the line's place (0 the left edge: all original, 1 the right: all remastered) over the segment's time T: the
    original, a sweep to the remastered across the whole screen, held, back to the original, and over again"""
    def ease(a, b):
        x = "((T-%.3f)/%.3f)" % (a, b - a)
        return "(%s*%s*(3-2*%s))" % (x, x, x)
    k = min(1.0, s / 7)  # (a shorter segment, every step shorter)
    t1, t2 = 0.5 * k, 1.8 * k
    t3 = max(t2 + 0.8 * k, s - 3.2 * k)
    t4, t5 = t3 + 1.1 * k, t3 + 1.6 * k
    t6 = t5 + 1.1 * k
    return ("if(lt(T,%.3f),0,if(lt(T,%.3f),%s,if(lt(T,%.3f),1,if(lt(T,%.3f),1-%s,if(lt(T,%.3f),0,if(lt(T,%.3f),%s,1))))))"
            % (t1, t2, ease(t1, t2), t3, t4, ease(t3, t4), t5, t6, ease(t5, t6)))


def rider(label, color, x, alpha):
    return ("drawtext=fontfile=bold.ttf:text='%s':fontsize=54:fontcolor=%s:x='%s':y=110:alpha='%s'"
            ":shadowcolor=black@0.6:shadowx=3:shadowy=3" % (label, color, x, alpha))


def segment(seg, clips, work, n):
    out = os.path.join(work, "seg_%03d.mp4" % n)
    seconds = float(seg.get("seconds", 5))
    fade = float(seg.get("fade", 0.25))
    fit = "scale=%d:%d:flags=lanczos,setsar=1,fps=%d" % (W, H, FPS)
    if "title" in seg:
        big = seg.get("size", 150)
        vf = [text(seg["title"], "bold.ttf", big, "(w-text_w)/2", "(h-text_h)/2-%d" % (90 if seg.get("sub") else 0), GOLD,
                   0.3, seconds - 0.3)]
        if seg.get("sub"):
            vf.append(text(seg["sub"], "bold.ttf", 110, "(w-text_w)/2", "(h-text_h)/2+60", "white", 0.7, seconds - 0.3))
        if seg.get("line"):
            vf.append(text(seg["line"], "mono.ttf", 40, "(w-text_w)/2", "(h-text_h)/2+190", "white", 1.1, seconds - 0.3, 0.8))
        run(["-f", "lavfi", "-i", "color=c=0x0B0B0A:s=%dx%d:r=%d:d=%.3f" % (W, H, FPS, seconds),
             "-vf", ",".join(vf + fades(seconds, fade))] + ENC + ["-t", "%.3f" % seconds, out], work)
    elif "clip" in seg:
        src = os.path.join(clips, seg["clip"] + ".mp4")
        vf = [fit] + caption(seg, seconds) + fades(seconds, fade)
        run(["-ss", "%.3f" % seg.get("start", 0), "-t", "%.3f" % seconds, "-i", src, "-vf", ",".join(vf), "-an"] + ENC
            + [out], work)
    elif "compare" in seg:
        on = os.path.join(clips, seg["compare"] + ".mp4")
        off = os.path.join(clips, seg["compare"] + "_off.mp4")
        s = seconds
        p = sweep(s)
        px = p.replace("T", "t")
        # the remastered take over the original, seen left of the line (its alpha a mask made per frame); each label
        # rides beside the line on its own side, gone as its side closes
        graph = (
            "[0:v]%s,format=yuva420p[on];[1:v]%s,format=yuv420p[off];"
            "nullsrc=s=%dx%d:r=%d:d=%.3f,format=gray,geq=lum='if(lt(X,W*(%s)),255,0)'[mask];"
            "[on][mask]alphamerge[onm];[off][onm]overlay=0:0:shortest=1[mix];"
            "color=c=white:s=4x%d:r=%d:d=%.3f[bar];"
            "[mix][bar]overlay=x='W*(%s)-2':y=0:shortest=1,format=yuv420p,"
            "%s,%s%s[v]"
            % (fit, fit, W, H, FPS, s, p, H, FPS, s, px,
               rider("REMASTERED", GOLD, "w*(%s)-text_w-48" % px, "clip((%s-0.16)/0.06,0,1)" % px),
               rider("ORIGINAL", "white", "w*(%s)+48" % px, "clip((0.86-(%s))/0.06,0,1)" % px),
               "".join("," + c for c in caption(seg, s) + fades(s, fade))))
        start = "%.3f" % seg.get("start", 0)
        run(["-ss", start, "-t", "%.3f" % s, "-i", on, "-ss", start, "-t", "%.3f" % s, "-i", off,
             "-filter_complex", graph, "-map", "[v]", "-an"] + ENC + [out], work)
    else:
        sys.exit("unknown segment: %r" % seg)
    return out, seconds


def trailer(edit_path, clips, out):
    edit = json.load(open(edit_path))
    work = os.path.join(os.path.dirname(os.path.abspath(out)), "segments")
    os.makedirs(work, exist_ok=True)
    shutil.copy(FONT_BOLD, os.path.join(work, "bold.ttf"))
    shutil.copy(FONT_MONO, os.path.join(work, "mono.ttf"))
    parts, total = [], 0.0
    for n, seg in enumerate(edit["segments"]):
        path, seconds = segment(seg, os.path.abspath(clips), work, n)
        parts.append(path)
        total += seconds
        print("%3d %-28s %5.1f s  (%.1f)" % (n, seg.get("clip") or seg.get("compare") or seg.get("title"), seconds, total),
              flush=True)
    with open(os.path.join(work, "list.txt"), "w") as f:
        for p in parts:
            f.write("file '%s'\n" % os.path.basename(p))
    music = os.path.join(ROOT, edit["music"])
    m0 = float(edit.get("music_start", 0))
    audio = "afade=t=in:st=0:d=1.5,afade=t=out:st=%.2f:d=3.5,volume=%.2f" % (total - 3.5, edit.get("volume", 1.0))
    run(["-f", "concat", "-safe", "0", "-i", "list.txt", "-ss", "%.2f" % m0, "-t", "%.3f" % total, "-i", music,
         "-map", "0:v", "-map", "1:a", "-c:v", "copy", "-af", audio, "-c:a", "aac", "-ar", "48000", "-b:a", "320k",
         "-movflags", "+faststart", "-shortest", os.path.abspath(out)], work)
    print("trailer: %.1f s -> %s" % (total, out))


def gif(clip, start, seconds, out, width):
    """a README GIF: a palette made for the clip, then the clip at 15 fps (ordered dither: still areas stay still
    from frame to frame, the file small)"""
    pal = out + ".palette.png"
    chain = "fps=15,scale=%d:-1:flags=lanczos" % width
    run(["-ss", str(start), "-t", str(seconds), "-i", clip, "-vf", chain + ",palettegen=max_colors=128:stats_mode=diff", pal], ".")
    run(["-ss", str(start), "-t", str(seconds), "-i", clip, "-i", pal, "-lavfi",
         chain + "[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle", out], ".")
    os.remove(pal)
    print("gif: %s (%.1f MB)" % (out, os.path.getsize(out) / 2**20))


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--gif":
        ap = argparse.ArgumentParser()
        ap.add_argument("--gif", nargs=4, metavar=("CLIP", "START", "SECONDS", "OUT"))
        ap.add_argument("--width", type=int, default=960)
        a = ap.parse_args()
        clip, start, seconds, out = a.gif
        gif(clip, float(start), float(seconds), out, a.width)
        return
    ap = argparse.ArgumentParser()
    ap.add_argument("edit")
    ap.add_argument("--clips", default=os.path.join(ROOT, "work", "trailer", "clips"))
    ap.add_argument("--out", default=os.path.join(ROOT, "work", "trailer", "trailer.mp4"))
    a = ap.parse_args()
    trailer(a.edit, a.clips, a.out)


if __name__ == "__main__":
    main()
