#!/usr/bin/env python3
"""PlayStation button glyphs for the game's PC key icons, game wide, and a set of glyph textures of our own.

    ps_glyphs.py GlobalB.lzc GlobalB.glyphs.lzc [preview.png]

The front end's button bars draw their key prompts with the PC key icons in GLOBAL/GlobalB.lzc's second texture
pack (PC_ENTER_ICON, PC_ESC_KEY_ICON, ...; 16x16, named only by the .fng packages). Each one named in REPLACE gets
the picture of the Vita button that does that job (64x64, 32-bit), so every menu shows PlayStation glyphs. ADD puts
VITA_GLYPH_* textures (hash: bStringHash of the name) in the same pack for our own HUD panels (roam_game.c).

Works on the stock file or on one vita_icon.py has patched (either order). The pack grows; it and everything after
it move by a multiple of 0x1000, its pixel data keeps its 0x80 alignment. Copy the result over GlobalB.lzc and
GLOBALB.BUN in the game folder.
"""
import struct, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

N = 64                                  # glyph textures: N x N, A8R8G8B8
SS = 8                                  # drawn at SS times, then scaled down

BLUE = (124, 178, 232); RED = (255, 102, 102); GREEN = (64, 226, 160); PINK = (255, 105, 248)
WHITE = (240, 240, 240); FILL = (20, 20, 24, 230)

def bhash(s):
    h = 0xFFFFFFFF
    for c in s.encode(): h = (h * 33 + c) & 0xFFFFFFFF
    return h

def _canvas(): return Image.new('RGBA', (N * SS, N * SS), (0, 0, 0, 0))
def _done(im): return im.resize((N, N), Image.LANCZOS)

def _font(px):
    for f in ('DejaVuSans-Bold.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 'arialbd.ttf'):
        try: return ImageFont.truetype(f, px)
        except OSError: pass
    return ImageFont.load_default()

def face(sym):
    im = _canvas(); d = ImageDraw.Draw(im); c = N * SS / 2; R = 29 * SS
    d.ellipse((c - R, c - R, c + R, c + R), fill=FILL, outline=WHITE + (255,), width=4 * SS)
    r = 13 * SS; w = 5 * SS
    if sym == 'cross':
        d.line((c - r, c - r, c + r, c + r), fill=BLUE + (255,), width=w); d.line((c - r, c + r, c + r, c - r), fill=BLUE + (255,), width=w)
    elif sym == 'circle':
        d.ellipse((c - r, c - r, c + r, c + r), outline=RED + (255,), width=w)
    elif sym == 'triangle':
        h = r * 1.1; d.polygon([(c, c - h), (c + h * .95, c + h * .65), (c - h * .95, c + h * .65)], outline=GREEN + (255,), width=w)
    elif sym == 'square':
        q = r * .9; d.rectangle((c - q, c - q, c + q, c + q), outline=PINK + (255,), width=w)
    return _done(im)

def pill(txt):
    im = _canvas(); d = ImageDraw.Draw(im)
    d.rounded_rectangle((3 * SS, 12 * SS, 61 * SS, 52 * SS), radius=12 * SS, fill=FILL, outline=WHITE + (255,), width=4 * SS)
    f = _font(int((26 if len(txt) < 3 else 15) * SS))
    bb = d.textbbox((0, 0), txt, font=f)
    d.text((N * SS / 2 - (bb[2] - bb[0]) / 2 - bb[0], N * SS / 2 - (bb[3] - bb[1]) / 2 - bb[1]), txt, font=f, fill=WHITE + (255,))
    return _done(im)

def dpad(lit):
    im = _canvas(); d = ImageDraw.Draw(im); c = N * SS / 2; a = 9 * SS; b = 27 * SS
    d.rectangle((c - a, c - b, c + a, c + b), fill=FILL, outline=WHITE + (255,), width=3 * SS)
    d.rectangle((c - b, c - a, c + b, c + a), fill=FILL, outline=WHITE + (255,), width=3 * SS)
    d.rectangle((c - a + 3 * SS, c - a + SS, c + a - 3 * SS, c + a - SS), fill=FILL)
    tri = {'up':    [(c, c - b + 4 * SS), (c - a + 3 * SS, c - a - 2 * SS), (c + a - 3 * SS, c - a - 2 * SS)],
           'down':  [(c, c + b - 4 * SS), (c - a + 3 * SS, c + a + 2 * SS), (c + a - 3 * SS, c + a + 2 * SS)],
           'left':  [(c - b + 4 * SS, c), (c - a - 2 * SS, c - a + 3 * SS), (c - a - 2 * SS, c + a - 3 * SS)],
           'right': [(c + b - 4 * SS, c), (c + a + 2 * SS, c - a + 3 * SS), (c + a + 2 * SS, c + a - 3 * SS)]}
    for k in lit: d.polygon(tri[k], fill=WHITE + (255,))
    return _done(im)

GLYPHS = {
    'CROSS': lambda: face('cross'), 'CIRCLE': lambda: face('circle'), 'TRIANGLE': lambda: face('triangle'),
    'SQUARE': lambda: face('square'), 'L': lambda: pill('L'), 'R': lambda: pill('R'),
    'START': lambda: pill('START'), 'SELECT': lambda: pill('SELECT'),
    'DPAD': lambda: dpad(''), 'UP': lambda: dpad(['up']), 'DOWN': lambda: dpad(['down']),
    'LEFT': lambda: dpad(['left']), 'RIGHT': lambda: dpad(['right']), 'LEFTRIGHT': lambda: dpad(['left', 'right']),
    'UPDOWN': lambda: dpad(['up', 'down']),
}

# The PC key icons and the Vita button doing that job (src/ug2/pad_defaults.cpp's menu map: Cross accept, Circle
# back, Triangle BUTTON1, Square BUTTON3, L/R the triggers). Keys whose job differs by menu are left out until
# their menus are read.
REPLACE = {
    'PC_ENTER_ICON': 'CROSS', 'PC_CONTINUE_ICON': 'CROSS',
    'PC_ESC_KEY_ICON': 'CIRCLE', 'PC_BACK_ICON': 'CIRCLE',
    'PC_LEFT_SQUARE_BRACKET_KEY_ICON': 'L', 'PC_RIGHT_SQUARE_BRACKET_KEY_ICON': 'R',
}
ADD = ['VITA_GLYPH_' + k for k in GLYPHS]      # our HUD panels' glyphs (roam_game.c GLYPH_*)

def chunks(d, o, end):
    while o + 8 <= end:
        cid, ln = struct.unpack_from('<II', d, o); yield cid, o, ln; o += 8 + ln

def ch(cid, body): return struct.pack('<II', cid, len(body)) + bytes(body)

def rgba_to_argb32(img):
    a = np.array(img.convert('RGBA'))
    return a[:, :, [2, 1, 0, 3]].tobytes()          # A8R8G8B8 in memory: B, G, R, A

def find_pack(d, name_hash):
    for cid, o, ln in chunks(d, 0, len(d)):
        if cid != 0xB3300000: continue
        for c2, o2, l2 in chunks(d, o + 8, o + 8 + ln):
            if c2 != 0xB3310000: continue
            for c3, o3, l3 in chunks(d, o2 + 8, o2 + 8 + l2):
                if c3 == 0x33310002 and name_hash in struct.unpack_from('<%dI' % (l3 // 4), d, o3 + 8)[0::2]:
                    return o, ln
    raise SystemExit('no texture pack with %08x' % name_hash)

def template32(d):
    """A 32-bit texture's info and format entries (WHITE16X16_PURE_WHITE), the kind ours are"""
    h = bhash('WHITE16X16_PURE_WHITE')
    o, ln = find_pack(d, h)
    info = {}
    for c2, o2, l2 in chunks(d, o + 8, o + 8 + ln):
        if c2 == 0xB3310000:
            for c3, o3, l3 in chunks(d, o2 + 8, o2 + 8 + l2): info[c3] = (o3 + 8, l3)
    hl = struct.unpack_from('<%dI' % (info[0x33310002][1] // 4), d, info[0x33310002][0])[0::2]
    i = hl.index(h)
    return bytes(d[info[0x33310004][0] + 124 * i:][:124]), bytes(d[info[0x33310005][0] + 32 * i:][:32])

def patch(src, dst, preview=None):
    d = bytearray(open(src, 'rb').read())
    tinfo32, fmt32 = template32(d)
    T, TL = find_pack(d, bhash('PC_ENTER_ICON'))
    kids = list(chunks(d, T + 8, T + 8 + TL))
    assert [k[0] for k in kids] == [0, 0xB3310000, 0, 0xB3320000], [hex(k[0]) for k in kids]
    (_, zo, zl), (_, io, il), (_, po, pl), (_, do, dl) = kids
    info = {c3: bytes(d[o3 + 8:o3 + 8 + l3]) for c3, o3, l3 in chunks(d, io + 8, io + 8 + il)}
    assert list(info) == [0x33310001, 0x33310002, 0x33310004, 0x33310005], [hex(k) for k in info]
    dkids = list(chunks(d, do + 8, do + 8 + dl))
    assert dkids[-1][0] == 0x33320002
    ro, rl = dkids[-1][1], dkids[-1][2]
    raw = bytearray(d[ro + 8:ro + 8 + rl])
    base_skip = ((ro + 8 + 0x7F) & ~0x7F) - (ro + 8)         # the raw data's lead to its 0x80 base
    n = len(info[0x33310002]) // 8
    hashes = list(struct.unpack_from('<%dI' % (2 * n), info[0x33310002])[0::2])
    tis = [bytearray(info[0x33310004][124 * i:124 * i + 124]) for i in range(n)]
    fms = [bytes(info[0x33310005][32 * i:32 * i + 32]) for i in range(n)]
    names = [t[12:36].split(b'\0')[0].decode('latin1') for t in tis]

    def pixels(img):
        nonlocal raw
        off = (len(raw) - base_skip + 0x7F) & ~0x7F
        raw += b'\0' * (base_skip + off - len(raw)) + rgba_to_argb32(img)
        return off

    def fill(ti, name, h, img):
        ti[:] = tinfo32
        ti[12:36] = name.encode()[:23].ljust(24, b'\0')
        struct.pack_into('<I', ti, 36, h)
        size = N * N * 4
        struct.pack_into('<I', ti, 48, pixels(img))
        struct.pack_into('<I', ti, 56, size); struct.pack_into('<I', ti, 64, size)
        struct.pack_into('<HH', ti, 68, N, N)
        ti[72] = ti[73] = N.bit_length() - 1
        ti[78] = 1                                          # one mip level

    made = {}
    for key, g in REPLACE.items():
        h = bhash(key)
        if h not in hashes:
            print('not in the pack, left:', key); continue
        img = made.setdefault(g, GLYPHS[g]())
        i = hashes.index(h)
        fill(tis[i], names[i], h, img); fms[i] = fmt32
        print('replaced', names[i], '->', g)
    for name in ADD:
        h = bhash(name)
        img = made.setdefault(name[11:], GLYPHS[name[11:]]())
        if h in hashes:
            i = hashes.index(h)
        else:
            i = sum(1 for x in hashes if x < h)
            hashes.insert(i, h); tis.insert(i, bytearray(124)); fms.insert(i, fmt32); names.insert(i, name)
        fill(tis[i], name, h, img); fms[i] = fmt32
        print('added', name, '%08x' % h)

    n = len(hashes)
    newinfo = (ch(0x33310001, info[0x33310001]) + ch(0x33310002, b''.join(struct.pack('<II', h, 0) for h in hashes))
               + ch(0x33310004, b''.join(bytes(t) for t in tis)) + ch(0x33310005, b''.join(fms)))
    head = ch(0, d[zo + 8:zo + 8 + zl]) + ch(0xB3310000, newinfo)
    # the zero chunk before the data: the data chunk moves by a multiple of 0x80 (its raw data keeps its base)
    old_data_at = do
    pad = pl
    while (T + 8 + len(head) + 8 + pad - old_data_at) % 0x80: pad += 1
    head += ch(0, b'\0' * pad)
    datahead = b''.join(ch(c, d[o + 8:o + 8 + l]) for c, o, l in dkids[:-1])
    def build():
        return ch(0xB3300000, head + ch(0xB3320000, datahead + ch(0x33320002, raw)))
    tpk = build()
    grow = len(tpk) - (8 + TL)
    raw += b'\0' * ((-grow) % 0x1000)
    tpk = build()
    assert (len(tpk) - (8 + TL)) % 0x1000 == 0
    out = bytes(d[:T]) + tpk + bytes(d[T + 8 + TL:])
    open(dst, 'wb').write(out)
    print('pack grew by 0x%x, %d textures' % (len(tpk) - (8 + TL), n))
    if preview:
        keys = list(made)
        sheet = Image.new('RGBA', (len(keys) * (N + 8) + 8, N + 16), (40, 40, 48, 255))
        for k, key in enumerate(keys): sheet.alpha_composite(made[key], (8 + k * (N + 8), 8))
        sheet.save(preview)

if __name__ == '__main__':
    patch(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
