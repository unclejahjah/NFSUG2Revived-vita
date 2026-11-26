"""Draws the Joypad picture with headless Chrome and writes its headers; --preview PNG renders it with English labels.
Art: Tokyoship's "PlayStation Vita illustration.svg", CC BY 3.0 (THIRD_PARTY.md)."""
import base64
import io
import os
import subprocess
import sys
import tempfile
from xml.sax.saxutils import escape

import picture_layout as pl

HERE = os.path.dirname(os.path.abspath(__file__))
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
SCREENSHOT = os.path.join(HERE, "screen.jpg")
SCREEN_BRIGHTNESS = 0.65
LEADER_COLOUR = "#cdcdcd"
XMLNS = ('xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
         'xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" '
         'xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd" '
         'xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:cc="http://creativecommons.org/ns#" '
         'xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#" xmlns:svg="http://www.w3.org/2000/svg"')
# The longest English text of each label, for the preview.
PREVIEW_TEXT = {
    "left_stick": "Steer Left / Steer Right", "right_stick": "Free Look", "touch_shop": "Enter Race / Shop",
    "touch_sms": "Display SMS", "touch_career": "Career Statistics", "touch_map": "World Map",
}


def dimmed_screenshot():
    """The dimmed screenshot as a data URI."""
    from PIL import Image
    _, _, w, h = pl.SCREEN
    img = Image.open(SCREENSHOT).convert("RGB").resize((int(w * 2), int(h * 2)), Image.LANCZOS)
    img = img.point(lambda v: int(v * SCREEN_BRIGHTNESS))
    data = io.BytesIO()
    img.save(data, "PNG")
    return "data:image/png;base64," + base64.b64encode(data.getvalue()).decode()


def shaded_art():
    """The illustration's content at art size (its own defs included)."""
    text = open(os.path.join(HERE, "vita_front_shaded.svg"), encoding="utf-8").read()
    return text[text.index(">", text.index("<svg")) + 1:text.rindex("</svg>")]


def body_svg(screen_uri):
    """The Vita in Front End units, screenshot shown, no caps."""
    x, y, w, h = pl.SCREEN
    return ('<g transform="translate(%r,%r) scale(%r)">%s<image href="%s" x="%g" y="%g" width="%g" height="%g" '
            'preserveAspectRatio="none"/></g>' % (pl.ART_X0, pl.ART_Y0, pl.ART_UNIT, shaded_art(), screen_uri, x, y, w, h))


def leader_svg(pts):
    """Light grey leader over a faint dark edge."""
    d = " ".join("%r,%r" % p for p in pts)
    return ('<polyline points="%s" fill="none" stroke="#000000" stroke-opacity="0.35" stroke-width="%g" '
            'stroke-linejoin="miter"/><polyline points="%s" fill="none" stroke="%s" stroke-width="%g" '
            'stroke-linejoin="miter"/>' % (d, pl.LEADER_WIDTH * 1.8, d, LEADER_COLOUR, pl.LEADER_WIDTH))


def labels_svg(lines):
    out = ['<g font-family="Helvetica" font-weight="bold" font-size="%g" fill="#ffffff">' % pl.EM]
    for name, (side, (x, y), _) in lines.items():
        out.append('<text x="%g" y="%g" text-anchor="%s">%s</text>' % (x, y + pl.EM * 0.35, "end" if side == "L" else "start",
                                                                      escape(PREVIEW_TEXT.get(name, "Hydraulics Bounce"))))
    out.append("</g>")
    return "".join(out)


def chrome(svg_path, png_path, width, height, profile):
    args = [CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--no-sandbox",
            "--default-background-color=00000000", "--allow-file-access-from-files", "--user-data-dir=" + profile,
            "--force-device-scale-factor=1", "--window-size=%d,%d" % (width, height), "--screenshot=" + png_path,
            "file://" + svg_path]
    try:
        subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=90)
    except subprocess.CalledProcessError:
        if not (os.path.exists(png_path) and os.path.getsize(png_path) > 0):     # Chrome's teardown can fail after it wrote the shot
            raise


def render(content, width, height, scale, x0, y0, profile, tmp, name):
    """Renders Front End content; (x0, y0) is the top left."""
    from PIL import Image
    svg = os.path.join(tmp, name + ".svg")
    png = os.path.join(tmp, name + ".png")
    text = ('<svg %s viewBox="0 0 %d %d" width="%d" height="%d"><g transform="scale(%r) translate(%r,%r)">%s</g></svg>'
            % (XMLNS, width, height, width, height, scale, -x0, -y0, content))
    open(svg, "w", encoding="utf-8").write(text)
    chrome(svg, png, width, height, profile)
    return Image.open(png).convert("RGBA")


def preview(path, profile, tmp):
    from PIL import Image
    k = 1.6
    x0, y0, x1, y1 = pl.CANVAS
    lines = pl.leaders()
    box = '<rect x="%g" y="%g" width="%g" height="%g" fill="none" stroke="#5a5a60" stroke-dasharray="4 4"/>' % (
        pl.BOX[0], pl.BOX[1], pl.BOX[2] - pl.BOX[0], pl.BOX[3] - pl.BOX[1])
    content = box + body_svg(dimmed_screenshot()) + "".join(leader_svg(p) for _, _, p in lines.values()) + labels_svg(lines)
    img = render(content, int((x1 - x0) * k), int((y1 - y0) * k), k, x0, y0, profile, tmp, "preview")
    out = Image.new("RGBA", img.size, (24, 24, 26, 255))
    out.alpha_composite(img)
    out.save(path)
    print("wrote", path)


def main():
    if not os.path.exists(CHROME):
        print("Chrome not found: nothing to render")
        return 1
    problems = pl.problems()
    if problems:
        print("layout problems:\n  " + "\n  ".join(problems))
        return 1
    import make_layers
    with tempfile.TemporaryDirectory() as profile, tempfile.TemporaryDirectory() as tmp:
        if "--preview" in sys.argv:
            preview(sys.argv[sys.argv.index("--preview") + 1], profile, tmp)
        else:
            make_layers.make_layers(dimmed_screenshot(), profile, tmp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
