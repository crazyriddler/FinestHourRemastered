"""Button prompts for the pack: Kenney's Input Prompts (CC0, kenney.nl/assets/input-prompts) at 128 pixels, with a soft
dark outline so they read on the menus' light paper and the game's dark scenes, as <pack>/prompts/<name>.dds (RGBA):

    kb_<vk>        a key or mouse button by its virtual key code, hex (wheel up 100, down 101)
    kb_updown, kb_leftright, kb_arrows, kb_wasd, kb_mouse
    pad_<kind>_<button>   kind xbox, ps, nin; button a b x y lb rb lt rt ls rs view menu

    python -I prompts.py <Kenney pack folder> <pack folder>
"""
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

S = 128

KEYS = {0x08: "keyboard_backspace", 0x09: "keyboard_tab", 0x0D: "keyboard_enter", 0x10: "keyboard_shift",
        0x11: "keyboard_ctrl", 0x12: "keyboard_alt", 0x14: "keyboard_capslock", 0x1B: "keyboard_escape",
        0x20: "keyboard_space", 0x25: "keyboard_arrow_left", 0x26: "keyboard_arrow_up", 0x27: "keyboard_arrow_right",
        0x28: "keyboard_arrow_down", 0x2E: "keyboard_delete", 0xA0: "keyboard_shift", 0xA1: "keyboard_shift",
        0xA2: "keyboard_ctrl", 0xA3: "keyboard_ctrl", 0xA4: "keyboard_alt", 0xA5: "keyboard_alt",
        0x01: "mouse_left", 0x02: "mouse_right", 0x04: "mouse_scroll", 0x05: "mouse_side_back",
        0x06: "mouse_side_forward", 0x100: "mouse_scroll_up", 0x101: "mouse_scroll_down"}
for c in range(26):
    KEYS[0x41 + c] = "keyboard_" + chr(ord("a") + c)
for d in range(10):
    KEYS[0x30 + d] = "keyboard_%d" % d
for f in range(12):
    KEYS[0x70 + f] = "keyboard_f%d" % (f + 1)

LABELS = {"kb_updown": "keyboard_arrows_vertical", "kb_leftright": "keyboard_arrows_horizontal",
          "kb_arrows": "keyboard_arrows_all", "kb_mouse": "mouse_move"}

PAD = {
    "xbox": ("Xbox Series", {"a": "xbox_button_color_a", "b": "xbox_button_color_b", "x": "xbox_button_color_x",
                             "y": "xbox_button_color_y", "lb": "xbox_lb", "rb": "xbox_rb", "lt": "xbox_lt",
                             "rt": "xbox_rt", "ls": "xbox_stick_l_press", "rs": "xbox_stick_r_press",
                             "view": "xbox_button_view", "menu": "xbox_button_menu"}),
    "ps": ("PlayStation Series", {"a": "playstation_button_cross", "b": "playstation_button_circle",
                                  "x": "playstation_button_square", "y": "playstation_button_triangle",
                                  "lb": "playstation_trigger_l1", "rb": "playstation_trigger_r1",
                                  "lt": "playstation_trigger_l2", "rt": "playstation_trigger_r2",
                                  "ls": "playstation_button_l3", "rs": "playstation_button_r3",
                                  "view": "playstation5_button_create", "menu": "playstation5_button_options"}),
    # Nintendo's letters by position: the south button is B
    "nin": ("Nintendo Switch", {"a": "switch_button_b", "b": "switch_button_a", "x": "switch_button_y",
                                "y": "switch_button_x", "lb": "switch_button_l", "rb": "switch_button_r",
                                "lt": "switch_button_zl", "rt": "switch_button_zr", "ls": "switch_stick_l_press",
                                "rs": "switch_stick_r_press", "view": "switch_button_minus",
                                "menu": "switch_button_plus"}),
}


def load(kenney, folder, name):
    path = os.path.join(kenney, folder, "Double", name + ".png")
    return Image.open(path).convert("RGBA").resize((S, S), Image.LANCZOS) if os.path.exists(path) else None


def outlined(im):
    """the icon over a soft dark outline (its alpha grown and blurred)"""
    a = im.getchannel("A")
    grown = a.filter(ImageFilter.MaxFilter(7)).filter(ImageFilter.GaussianBlur(2.5))
    shadow = Image.new("RGBA", im.size, (12, 12, 14, 0))
    shadow.putalpha(grown.point(lambda v: int(v * 0.75)))
    shadow.alpha_composite(im)
    return shadow


def wasd(kenney):
    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    k = S * 9 // 20
    for letter, (x, y) in {"w": (S // 2 - k // 2, 2), "a": (S // 2 - k * 3 // 2, S - k - 4),
                           "s": (S // 2 - k // 2, S - k - 4), "d": (S // 2 + k // 2, S - k - 4)}.items():
        im = load(kenney, "Keyboard & Mouse", "keyboard_" + letter).resize((k, k), Image.LANCZOS)
        out.alpha_composite(im, (x, y))
    return out


def playstation(symbol):
    """the PlayStation face buttons as on the pad: a dark grey button, the symbol in its colour as a line"""
    big = 1024
    im = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    m = big * 0.03
    d.ellipse((m, m, big - m, big - m), fill=(99, 99, 99, 255))
    w = int(big * 0.075)  # the symbol's line
    c = big / 2
    if symbol == "cross":
        r = big * 0.25
        col = (127, 215, 255, 255)
        d.line((c - r, c - r, c + r, c + r), fill=col, width=w)
        d.line((c - r, c + r, c + r, c - r), fill=col, width=w)
    elif symbol == "circle":
        r = big * 0.285
        d.ellipse((c - r, c - r, c + r, c + r), outline=(255, 85, 85, 255), width=w)
    elif symbol == "square":
        r = big * 0.255
        d.rectangle((c - r, c - r, c + r, c + r), outline=(255, 134, 174, 255), width=w)
    else:  # triangle
        r = big * 0.31
        pts = [(c, c - r), (c + r * 0.866, c + r * 0.5), (c - r * 0.866, c + r * 0.5)]
        tri = Image.new("L", (big, big), 0)
        td = ImageDraw.Draw(tri)
        td.polygon(pts, fill=255)
        k = 1 - w * 2.0 / r  # the inner triangle, the line's width in
        td.polygon([(c + (x - c) * k, c + (y - c) * k) for x, y in pts], fill=0)  # same centre, inradius less w
        im.paste((107, 240, 177, 255), (0, 0), tri)
    return im.resize((S, S), Image.LANCZOS)


def save_dds(path, im):
    w, h = im.size
    data = np.asarray(im.convert("RGBA"), np.uint8).tobytes()
    head = struct.pack("<4sIIIIIII44x", b"DDS ", 124, 0x1 | 0x2 | 0x4 | 0x8 | 0x1000, h, w, w * 4, 0, 1)
    pf = struct.pack("<II4s20x", 32, 0x4, b"DX10")
    caps = struct.pack("<IIII4x", 0x1000, 0, 0, 0)
    dx10 = struct.pack("<IIIII", 28, 3, 0, 1, 0)  # R8G8B8A8_UNORM, 2D
    with open(path, "wb") as f:
        f.write(head + pf + caps + dx10 + data)


def main():
    kenney, pack = sys.argv[1:3]
    out = os.path.join(pack, "prompts")
    os.makedirs(out, exist_ok=True)
    n = 0
    for vk, name in KEYS.items():
        im = load(kenney, "Keyboard & Mouse", name)
        if im:
            save_dds(os.path.join(out, "kb_%02x.dds" % vk), outlined(im))
            n += 1
    for tag, name in LABELS.items():
        save_dds(os.path.join(out, tag + ".dds"), outlined(load(kenney, "Keyboard & Mouse", name)))
        n += 1
    save_dds(os.path.join(out, "kb_wasd.dds"), outlined(wasd(kenney)))
    n += 1
    for kind, (folder, buttons) in PAD.items():
        for button, name in buttons.items():
            symbol = {"a": "cross", "b": "circle", "x": "square", "y": "triangle"}.get(button)
            im = playstation(symbol) if kind == "ps" and symbol else load(kenney, folder, name)
            if im:
                save_dds(os.path.join(out, "pad_%s_%s.dds" % (kind, button)), outlined(im))
                n += 1
    with open(os.path.join(out, "LICENSE.txt"), "w") as f:
        f.write("Input Prompts by Kenney (www.kenney.nl), CC0 1.0 Universal: "
                "https://creativecommons.org/publicdomain/zero/1.0/\n")
    print("%d prompts" % n)


if __name__ == "__main__":
    main()
