"""Renders the README's Control Type 1 Joypad picture and rear touch pad diagram (needs Chrome, Pillow, the game's English.bin).
Usage: make_readme_images.py [game dir]. Writes readme_controls_type1.png, readme_rear_pad.png."""
import os
import struct
import sys
import tempfile
from xml.sax.saxutils import escape

import make_diagrams as md
import picture_layout as pl

HERE = md.HERE
GAME_DIR = "/Users/danuka/Downloads/Need_for_Speed_Underground_2_Win_Installed"
FONT = 'font-family="Helvetica Neue, Helvetica, Arial" font-style="italic" font-weight="bold"'
PANEL, EDGE = "#16171a", "#3a3b42"
HASH = {"Accelerate": 0x619b23b0, "Steer": 0x63aa639d, "Handbrake": 0xb70ff4a4, "Nitrous": 0x5e5c5f2e,
        "Shift Up": 0xdc1cd97d, "Shift Down": 0x56afe190, "Change Camera": 0xd38df194, "Enter Race / Shop": 0x4a1341b3,
        "Display SMS": 0x6ef09d0d, "World Map": 0x6026c91f, "Career Statistics": 0x70a30056,
        "Hydraulics Bounce": 0x64730aac, "Hydraulics": 0xeac74624}
TYPE1 = {"triangle": "Shift Up", "circle": "Nitrous", "cross": "Handbrake", "square": "Shift Down", "l": "Brake",
         "r": "Accelerate", "dpad_up": "Enter Race / Shop", "dpad_right": "Display SMS", "dpad_down": "Change Camera",
         "dpad_left": "World Map", "left_stick": "Steer", "right_stick": "Free Look", "touch_career": "Career Statistics"}
OUT_W = 960


def game_texts(game_dir):
    """Hash -> text of the game's English table."""
    d = open(os.path.join(game_dir, "LANGUAGES", "English.bin"), "rb").read()
    base = struct.unpack_from("<I", d, 20)[0] + 8
    out = {}
    for k in range(0x1814, base, 8):
        h, o = struct.unpack_from("<II", d, k)
        out[h] = d[base + o:d.index(b"\0", base + o)].decode("latin1")
    return out


def game_text(texts, name):
    """The game's own text for an action, our English where it has none."""
    return texts.get(HASH.get(name), name)


def joypad(texts, profile, tmp):
    from PIL import Image
    x0, y0, x1, y1 = pl.BOX[0], pl.BOX[1] + 14, pl.BOX[2], 302.0
    k = OUT_W / (x1 - x0)
    lines = pl.leaders()
    body = ['<rect x="%g" y="%g" width="%g" height="%g" rx="10" fill="%s" stroke="%s" stroke-width="1.5"/>'
            % (x0, y0, x1 - x0, y1 - y0, PANEL, EDGE), md.body_svg(md.dimmed_screenshot())]
    text = ['<g %s font-size="%g" fill="#ffffff">' % (FONT, pl.EM)]
    for name, label in TYPE1.items():
        side, (lx, ly), pts = lines[name]
        body.append(md.leader_svg(pts))
        text.append('<text x="%g" y="%g" text-anchor="%s">%s</text>' % (
            lx, ly + pl.EM * 0.35, "end" if side == "L" else "start", escape(game_text(texts, label))))
    text.append("</g>")
    text.append('<text x="%g" y="%g" text-anchor="middle" %s font-size="14" fill="#cdcdcd">Control Type 1</text>' % (
        0, y0 + 20, FONT))
    img = md.render("".join(body + text), OUT_W, int((y1 - y0) * k), k, x0, y0, profile, tmp, "joypad")
    return img


def rear_pad(texts, profile, tmp):
    W, H = 960, 520
    hyd, bounce = game_text(texts, "Hydraulics"), game_text(texts, "Hydraulics Bounce")
    px, py, pw, ph = 180, 110, 600, 300
    quads = [("L2", "not assigned", 0, 0, "#4a5a7a"), ("R2", "not assigned", 1, 0, "#4a5a7a"),
             ("L3", hyd, 0, 1, "#e0a030"), ("R3", bounce, 1, 1, "#e0a030")]
    s = ['<rect width="%d" height="%d" rx="14" fill="%s" stroke="%s" stroke-width="2"/>' % (W, H, PANEL, EDGE),
         '<text x="%d" y="52" text-anchor="middle" %s font-size="26" fill="#ffffff">Rear touch pad</text>' % (W // 2, FONT),
         '<text x="%d" y="82" text-anchor="middle" font-family="Helvetica" font-size="16" fill="#9a9aa4">'
         'Four quadrants, as you hold the Vita (Control Types 1 to 4)</text>' % (W // 2),
         '<rect x="%d" y="%d" width="%d" height="%d" rx="46" fill="#26272c" stroke="#5a5a62" stroke-width="3"/>' % (
             px - 40, py - 14, pw + 80, ph + 28)]
    for name, text, cx, cy, col in quads:
        x, y = px + cx * pw / 2, py + cy * ph / 2
        used = cy == 1
        s.append('<rect x="%g" y="%g" width="%g" height="%g" fill="%s" fill-opacity="%s"/>' % (
            x, y, pw / 2, ph / 2, col, "0.30" if used else "0.18"))
        s.append('<text x="%g" y="%g" text-anchor="middle" %s font-size="34" fill="#ffffff">%s</text>' % (
            x + pw / 4, y + ph / 4 - 4, FONT, name))
        s.append('<text x="%g" y="%g" text-anchor="middle" %s font-size="%d" fill="%s">%s</text>' % (
            x + pw / 4, y + ph / 4 + 30, FONT, 21 if used else 17, "#ffffff" if used else "#9a9aa4", escape(text)))
    s.append('<rect x="%d" y="%d" width="%d" height="%d" rx="8" fill="none" stroke="#c9c9d0" stroke-width="2.5"/>' % (px, py, pw, ph))
    s.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="#c9c9d0" stroke-width="2" stroke-dasharray="7 6"/>' % (px + pw // 2, py, px + pw // 2, py + ph))
    s.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="#c9c9d0" stroke-width="2" stroke-dasharray="7 6"/>' % (px, py + ph // 2, px + pw, py + ph // 2))
    s.append('<text x="%d" y="%d" text-anchor="middle" font-family="Helvetica" font-size="16" fill="#9a9aa4">'
             'Presets: L3 is %s, R3 is %s; L2 and R2 are free.</text>' % (W // 2, 470, escape(hyd), escape(bounce)))
    return md.render("".join(s), W, H, 1, 0, 0, profile, tmp, "rear")


def save(img, path):
    from PIL import Image
    flat = Image.new("RGB", img.size, (24, 24, 26))
    flat.paste(img, mask=img.split()[3])
    flat.quantize(colors=128, method=Image.MEDIANCUT, dither=Image.NONE).save(path, optimize=True)
    print("wrote", path, os.path.getsize(path))


def main():
    game_dir = sys.argv[1] if len(sys.argv) > 1 else GAME_DIR
    texts = game_texts(game_dir)
    with tempfile.TemporaryDirectory() as profile, tempfile.TemporaryDirectory() as tmp:
        save(joypad(texts, profile, tmp), os.path.join(HERE, "readme_controls_type1.png"))
        save(rear_pad(texts, profile, tmp), os.path.join(HERE, "readme_rear_pad.png"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
