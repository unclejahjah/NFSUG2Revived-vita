"""Draws the PS2-style button icons as vectors, renders them with headless Chrome, writes src/ug2/helpicons_art.h.

Needs Pillow and Chrome. ps2/ keeps the PS2 originals for reference.
"""
import concurrent.futures
import math
import os
import struct
import subprocess
import sys
import tempfile

from PIL import Image, ImageChops

HERE = os.path.dirname(os.path.abspath(__file__))
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
LIME = (202, 244, 138)
ATLAS = 256
SIZE = 64
COLUMNS = ATLAS // SIZE
SUPER = 16                  # render scale before the box-filter downsample
STROKE = 4                  # line weight of every 64x64 icon
DIM = 0.7                   # opacity of the d-pad arms that are not meant

ICONS = ["CROSS", "CIRCLE", "SQUARE", "TRIANGLE", "L", "R", "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT",
         "LSTICK", "RSTICK", "START", "SELECT"]
REAR_CELL = len(ICONS)      # four rear-pad icons, two by two
ARM_CELL = REAR_CELL + 1    # four 32x32 single-arm d-pad icons, two by two
ARMS = ("UP", "DOWN", "LEFT", "RIGHT")
REAR_W, REAR_H, REAR_PITCH = 30, 20, 20     # the pad is 28x18 (14:9) inside, one texel of margin all round
PAD_W, PAD_H, PAD_STROKE, PAD_RADIUS = 28, 18, 2, 2
ARM = 32


def f(v):
    return ("%.4f" % v).rstrip("0").rstrip(".")


def path(points):
    return "M" + " L".join("%s,%s" % (f(x), f(y)) for x, y in points) + " Z"


def inset(points, d):
    """A convex polygon moved inwards by d on every edge."""
    n = len(points)
    area = sum(points[i][0] * points[(i + 1) % n][1] - points[(i + 1) % n][0] * points[i][1] for i in range(n))
    sign = 1 if area > 0 else -1
    lines = []
    for i in range(n):
        (x0, y0), (x1, y1) = points[i], points[(i + 1) % n]
        length = math.hypot(x1 - x0, y1 - y0)
        nx, ny = -(y1 - y0) / length * sign, (x1 - x0) / length * sign
        lines.append(((x0 + nx * d, y0 + ny * d), (x1 - x0, y1 - y0)))
    out = []
    for i in range(n):
        (p, r), (q, s) = lines[i - 1], lines[i]
        t = ((q[0] - p[0]) * s[1] - (q[1] - p[1]) * s[0]) / (r[0] * s[1] - r[1] * s[0])
        out.append((p[0] + t * r[0], p[1] + t * r[1]))
    return out


def outline(points, width, opacity=1.0):
    """Convex outline whose outer edge is the polygon."""
    return '<path fill-rule="evenodd" fill-opacity="%s" d="%s %s"/>' % (f(opacity), path(points), path(inset(points, width)))


def ring(cx, cy, outer, width):
    return '<circle cx="%s" cy="%s" r="%s" fill="none" stroke="#fff" stroke-width="%s"/>' % (
        f(cx), f(cy), f(outer - width / 2), f(width))


def rounded_frame(x0, y0, x1, y1, radius, width):
    """Rounded rectangle outline, outer edge on the given box."""
    r = radius
    outer = ("M%s,%s H%s A%s,%s 0 0 1 %s,%s V%s A%s,%s 0 0 1 %s,%s H%s A%s,%s 0 0 1 %s,%s V%s A%s,%s 0 0 1 %s,%s Z"
             % tuple(map(f, (x0 + r, y0, x1 - r, r, r, x1, y0 + r, y1 - r, r, r, x1 - r, y1, x0 + r, r, r, x0, y1 - r,
                             y0 + r, r, r, x0 + r, y0))))
    inner = path([(x0 + width, y0 + width), (x1 - width, y0 + width), (x1 - width, y1 - width), (x0 + width, y1 - width)])
    return '<path fill-rule="evenodd" d="%s %s"/>' % (outer, inner)


# Face buttons: one ring for all four, symbols with the same stroke, sized and centred by eye.
FACE_RING = 24          # outer radius; the ring spans 8..56


def face(symbol):
    parts = [ring(32, 32, FACE_RING, STROKE)]
    if symbol == "CROSS":
        a = 11
        for x0, y0, x1, y1 in ((32 - a, 32 - a, 32 + a, 32 + a), (32 + a, 32 - a, 32 - a, 32 + a)):
            parts.append('<line x1="%s" y1="%s" x2="%s" y2="%s" stroke="#fff" stroke-width="%s"/>'
                         % (f(x0), f(y0), f(x1), f(y1), f(STROKE)))
    elif symbol == "CIRCLE":
        parts.append(ring(32, 32, 14, STROKE))
    elif symbol == "SQUARE":
        parts.append(outline([(20, 20), (44, 20), (44, 44), (20, 44)], STROKE))
    elif symbol == "TRIANGLE":
        half, base = 14, 42
        apex = base - half * math.sqrt(3)
        parts.append(outline([(32, apex), (32 + half, base), (32 - half, base)], STROKE))
    return parts


# L and R: one hand-drawn monoline face (no font file), cap height 20, stroke 4, used by the shoulders and sticks.
CAP = 20


def glyph(letter, x, y):
    s = STROKE
    if letter == "L":
        d = path([(0, 0), (s, 0), (s, CAP - s), (14, CAP - s), (14, CAP), (0, CAP)])
    else:
        bowl = 6
        leg_dx = 6.0
        leg_w = s * math.hypot(leg_dx, CAP / 2) / (CAP / 2)
        d = ("M0,0 H10 A{b},{b} 0 0 1 10,12 H{s} V{cap} H0 Z M{s},{s} V8 H10 A2,2 0 0 0 10,{s} Z ".format(
            b=bowl, s=s, cap=CAP)
             + path([(16 - leg_w - leg_dx, 10), (16 - leg_dx, 10), (16, CAP), (16 - leg_w, CAP)]))
    return '<path fill-rule="nonzero" transform="translate(%s,%s)" d="%s"/>' % (f(x), f(y), d)


GLYPH_WIDTH = {"L": 14, "R": 16}


def letter(ch, cy=32):
    return glyph(ch, 32 - GLYPH_WIDTH[ch] / 2, cy - CAP / 2)


def shoulder(ch):
    return [rounded_frame(3, 14, 61, 50, 2, STROKE), letter(ch)]


def stick(ch):
    return [ring(32, 32, 28, STROKE), letter(ch)]


def start():
    return [outline([(7, 17), (58, 32), (7, 47)], STROKE)]


def select():
    return [rounded_frame(7, 19, 57, 45, 2, STROKE)]


# The d-pad: one up arm (horizontal, vertical and 45 degree edges, blunt tip), mirrored and turned into the other three.
DPAD = (SIZE, 11, 6, 18, 2, 3)      # size, arm half width, arm top, taper start, tip half width, outline width


def arm_up(size, hw, top, taper, tip_hw):
    c, tip = size / 2, taper + hw - tip_hw
    return [(c - hw, top), (c + hw, top), (c + hw, taper), (c + tip_hw, tip), (c - tip_hw, tip), (c - hw, taper)]


def arm_parts(size, hw, top, taper, tip_hw, width):
    """Up arm filled at x 0, outlined at x size."""
    shape = arm_up(size, hw, top, taper, tip_hw)
    return ['<path d="%s"/>' % path(shape),
            '<g transform="translate(%d,0)">%s</g>' % (size, outline(shape, width, DIM))]


def mirrored(arm, size):
    """The arm's left half and its mirror: exactly symmetric texels."""
    out = arm.copy()
    out.paste(arm.crop((0, 0, size // 2, size)).transpose(Image.FLIP_LEFT_RIGHT), (size // 2, 0))
    return out


def dpad(filled, arms, size):
    """Four arms flipped from the up arm, meant ones filled."""
    out = Image.new("L", (size, size), 0)
    for direction in ARMS:
        up = arms[direction in filled]
        turned = {"UP": up, "DOWN": up.transpose(Image.FLIP_TOP_BOTTOM), "LEFT": up.transpose(Image.TRANSPOSE),
                  "RIGHT": up.transpose(Image.TRANSPOSE).transpose(Image.FLIP_LEFT_RIGHT)}[direction]
        out = ImageChops.lighter(out, turned)
    return out


def rear(quadrant):
    x0, y0, x1, y1 = 1, 1, 1 + PAD_W, 1 + PAD_H
    mx, my = (x0 + x1) / 2, (y0 + y1) / 2
    w = PAD_STROKE
    qx0, qx1 = (x0, mx) if quadrant % 2 == 0 else (mx, x1)
    qy0, qy1 = (y0, my) if quadrant < 2 else (my, y1)
    clip = "rear%d" % quadrant
    return ['<clipPath id="%s"><rect x="%s" y="%s" width="%s" height="%s" rx="%s"/></clipPath>'
            % (clip, f(x0), f(y0), f(PAD_W), f(PAD_H), f(PAD_RADIUS)),
            rounded_frame(x0, y0, x1, y1, PAD_RADIUS, w),
            '<rect x="%s" y="%s" width="%s" height="%s"/>' % (f(mx - w / 2), f(y0), f(w), f(PAD_H)),
            '<rect x="%s" y="%s" width="%s" height="%s"/>' % (f(x0), f(my - w / 2), f(PAD_W), f(w)),
            '<rect clip-path="url(#%s)" x="%s" y="%s" width="%s" height="%s"/>'
            % (clip, f(qx0), f(qy0), f(qx1 - qx0), f(qy1 - qy0))]


def cell_parts(name):
    if name in ("CROSS", "CIRCLE", "SQUARE", "TRIANGLE"):
        return face(name)
    if name in ("L", "R"):
        return shoulder(name)
    if name in ("LSTICK", "RSTICK"):
        return stick(name[0])
    return start() if name == "START" else select()


def cells():
    """Each cell's atlas position and its shapes."""
    out = [((n % COLUMNS) * SIZE, (n // COLUMNS) * SIZE, cell_parts(name)) for n, name in enumerate(ICONS)
           if not name.startswith("DPAD_")]
    rears = []
    for q in range(4):
        rears += ['<g transform="translate(%d,%d)">' % ((q % 2) * ARM, (q // 2) * REAR_PITCH)] + rear(q) + ["</g>"]
    out.append(((REAR_CELL % COLUMNS) * SIZE, (REAR_CELL // COLUMNS) * SIZE, rears))
    return out


def cell_svg(parts):
    """A cell's SVG, white on black: luminance is alpha."""
    return "\n".join(['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">' % ((ATLAS,) * 4),
                      '<rect width="%d" height="%d" fill="#000"/>' % (ATLAS, ATLAS), '<g fill="#fff" stroke="none">']
                     + parts + ["</g></svg>"])


def chrome(svg_path, png_path, profile):
    args = [CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--no-sandbox",
            "--allow-file-access-from-files", "--user-data-dir=" + profile, "--force-device-scale-factor=%d" % SUPER,
            "--window-size=%d,%d" % (ATLAS, ATLAS), "--screenshot=" + png_path, "file://" + svg_path]
    try:
        subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
    except subprocess.CalledProcessError:
        if not (os.path.exists(png_path) and os.path.getsize(png_path) > 0):     # Chrome's teardown can fail after the shot
            raise


def render_cell(parts, tmp, n, width=SIZE):
    """One cell, at the page origin so shared shapes match."""
    svg, png, profile = (os.path.join(tmp, "cell%d%s" % (n, ext)) for ext in (".svg", ".png", "-profile"))
    open(svg, "w").write(cell_svg(parts))
    chrome(svg, png, profile)
    big = Image.open(png).convert("RGB").getchannel("G")
    if big.size != (ATLAS * SUPER, ATLAS * SUPER):
        raise SystemExit("Chrome rendered %dx%d, expected %d" % (big.size + (ATLAS * SUPER,)))
    return big.crop((0, 0, width * SUPER, SIZE * SUPER)).reduce(SUPER)


def render_dpads(tmp):
    """Full d-pads by name; single-arm icons are the same arms halved."""
    both = render_cell(arm_parts(*DPAD), tmp, 100, 2 * SIZE)
    arms = {k: mirrored(both.crop((x, 0, x + SIZE, SIZE)), SIZE) for k, x in ((True, 0), (False, SIZE))}
    pads = {name: dpad(("UP", "DOWN") if name in ("DPAD_UP", "DPAD_DOWN") else ("LEFT", "RIGHT"), arms, SIZE)
            for name in ("DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT")}
    halves = {k: arm.reduce(2) for k, arm in arms.items()}
    pads["ARMS"] = [dpad((arm,), halves, ARM) for arm in ARMS]
    return pads


def render_alpha():
    """Atlas alpha: cells rendered at SUPER, then box filtered."""
    alpha = Image.new("L", (ATLAS, ATLAS), 0)
    with tempfile.TemporaryDirectory() as tmp, concurrent.futures.ThreadPoolExecutor(8) as pool:
        layout = cells()
        pads = pool.submit(render_dpads, tmp)
        for (x, y, _), img in zip(layout, pool.map(lambda c: render_cell(c[1][2], tmp, c[0]), enumerate(layout))):
            alpha.paste(img, (x, y))
        pads = pads.result()
    for name, img in pads.items():
        if name != "ARMS":
            n = ICONS.index(name)
            alpha.paste(img, ((n % COLUMNS) * SIZE, (n // COLUMNS) * SIZE))
    bx, by = (ARM_CELL % COLUMNS) * SIZE, (ARM_CELL // COLUMNS) * SIZE
    for q, img in enumerate(pads["ARMS"]):
        alpha.paste(img, (bx + (q % 2) * ARM, by + (q // 2) * ARM))
    return alpha


def rgb565(c):
    return ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)


def unpack565(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def dist(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b))


def dxt3(img):
    width, height = img.size
    pixels = img.load()
    out = bytearray()
    for by in range(0, height, 4):
        for bx in range(0, width, 4):
            block = [pixels[bx + i % 4, by + i // 4] for i in range(16)]
            for i in range(0, 16, 2):
                a0 = (block[i][3] * 15 + 127) // 255
                a1 = (block[i + 1][3] * 15 + 127) // 255
                out.append(a0 | (a1 << 4))
            best, first, second = -1, 0, 0
            for i in range(16):
                for j in range(i + 1, 16):
                    d = dist(block[i][:3], block[j][:3])
                    if d > best:
                        best, first, second = d, i, j
            c0, c1 = rgb565(block[first][:3]), rgb565(block[second][:3])
            if c0 < c1:
                c0, c1 = c1, c0
            e0, e1 = unpack565(c0), unpack565(c1)
            palette = [e0, e1, tuple((2 * a + b) // 3 for a, b in zip(e0, e1)),
                       tuple((a + 2 * b) // 3 for a, b in zip(e0, e1))]
            indices = 0
            for i in range(16):
                k = min(range(4), key=lambda n: dist(block[i][:3], palette[n]))
                indices |= (0 if c0 == c1 else k) << (2 * i)
            out += struct.pack("<HHI", c0, c1, indices)
    return bytes(out)


def build_atlas():
    alpha = render_alpha()
    atlas = Image.new("RGBA", (ATLAS, ATLAS), LIME + (0,))
    atlas.putalpha(alpha)
    return atlas


def main():
    if not os.path.exists(CHROME):
        print("Chrome not found: nothing to render")
        return 1
    atlas = build_atlas()
    names = list(ICONS) + ["REAR_" + q for q in ("UL", "UR", "LL", "LR")] + ["ARM_" + a for a in ARMS]
    data = dxt3(atlas)
    header = os.path.join(HERE, "..", "..", "src", "ug2", "helpicons_art.h")
    lines = ["// Generated by images/ps2help/make_icons.py: the PS2-style front-end button icons, redrawn as vectors, a %dx%d DXT3 atlas of %dx%d cells."
             % (ATLAS, ATLAS, SIZE, SIZE),
             "// Cells in order: " + ", ".join(names) + " (REAR_* are %dx%d with a 14:9 pad inside, two by two with rows %d texels apart and columns 32 apart, in the cell after SELECT; ARM_* are 32x32 d-pad icons with one arm filled, two by two in the next cell)."
             % (REAR_W, REAR_H, REAR_PITCH),
             "#ifndef UG2_HELPICONS_ART_H_", "#define UG2_HELPICONS_ART_H_", "", "#include <cstdint>", "",
             "namespace ug2 { namespace helpiconsart {", "", "const int Size = %d;" % ATLAS,
             "const int Cell = %d;" % SIZE, "const uint8_t Atlas[%d] =\n{" % len(data)]
    for i in range(0, len(data), 32):
        lines.append("    " + ",".join("0x%02x" % b for b in data[i:i + 32]) + ",")
    lines.append("};\n\n}}\n\n#endif /* !UG2_HELPICONS_ART_H_ */")
    with open(header, "w") as out:
        out.write("\n".join(lines) + "\n")
    bg = Image.new("RGBA", atlas.size, (60, 60, 64, 255))
    bg.alpha_composite(atlas)
    bg.resize((ATLAS * 2, ATLAS * 2), Image.NEAREST).save(os.path.join(HERE, "icons_preview.png"))
    print("wrote", header, names)
    return 0


if __name__ == "__main__":
    sys.exit(main())
