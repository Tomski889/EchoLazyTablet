"""Render the built PLAY tab to PNG, so the layout can be judged without a headset.

Reads the canvases build_queue_tab.py wrote (build/tab), draws sprites from the game's own
UI atlas and the PLAY tab icon, and fills the runtime's dynamic text with sample values for
each page state. Only the typeface is approximated (Echo's tablet font is baked; this uses
Inconsolata Bold, which ships with Echo).

    python preview_tab.py --font "<...>\\content\\engine\\core\\fonts\\Inconsolata-Bold.ttf"
"""
import argparse
import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUILD = HERE.parent / 'build/tab'

a = argparse.ArgumentParser()
a.add_argument('--font', required=True)
a.add_argument('--tools', required=True, help="Doom-on-EchoVR's EchoVr-Tablet-Probing folder (canvas reader)")
a.add_argument('--out', default=str(BUILD / 'preview'))
a = a.parse_args()
sys.path.insert(0, a.tools)
from canvas_edit import Canvas  # noqa: E402
from PIL import Image, ImageDraw, ImageFont  # noqa: E402

TEXT, PANEL, SPRITE, CHILD = 1, 2, 3, 7
PAGE_X, PAGE_Y = 12, 112   # as build_queue_tab.py
SCREEN_UV = (0.015, 0.232, 0.515, 0.456)   # the stock tablet screen background (a stock page's element 0)
meta = json.loads((BUILD / 'queue_tab.json').read_text())
E = meta['elements']
atlas = Image.open(BUILD / 'atlas.png').convert('RGBA')
icon = Image.open(BUILD / 'tab_icon.png').convert('RGBA')
TEXTURES = {int(meta['atlas'], 16): atlas, int(meta['icon_texture'], 16): icon,
            int(meta['row_atlas'], 16): Image.open(BUILD / 'row_atlas.png').convert('RGBA')}
fonts = {}


def font(size):
    if size not in fonts:
        fonts[size] = ImageFont.truetype(a.font, size)
    return fonts[size]


def literals(canvas):
    out = {}
    for i, row in enumerate(canvas.elements):
        if struct.unpack_from('<I', row, 8)[0] != TEXT:
            continue
        slot, cap = struct.unpack_from('<II', row, 0x90)
        start = struct.unpack_from('<I', canvas.string_offsets, slot * 4)[0]
        out[i] = bytes(canvas.text[start:start + cap]).split(b'\0')[0].decode('utf-8', 'replace')
    return out


def tinted(img, rgba):
    r, g, b, al = img.split()
    f = [max(0, min(1, v)) for v in rgba]
    return Image.merge('RGBA', [c.point(lambda v, k=k: round(v * k)) for c, k in zip((r, g, b, al), f)])


def draw_canvas(base, canvas, ox, oy, visible=None, texts=None):
    """Draw a canvas at (ox, oy). `visible` / `texts` override hidden flags and strings by index."""
    visible, texts = visible or {}, texts or {}
    w, h = struct.unpack_from('<2I', canvas.header, 0x14)
    lit = literals(canvas)
    quads = {}
    d = ImageDraw.Draw(base)
    for i, row in enumerate(canvas.elements):
        kind = struct.unpack_from('<I', row, 8)[0]
        hidden = struct.unpack_from('<I', row, 0x10)[0] != 0
        parent = struct.unpack_from('<i', row, 0x5c)[0]
        px0, py0, px1, py1 = quads[parent] if parent >= 0 else (ox, oy, ox + w, oy + h)
        ax0, ay0, ax1, ay1 = struct.unpack_from('<4f', row, 0x24)
        r0, r1, r2, r3 = struct.unpack_from('<4f', row, 0x34)
        pw, ph = px1 - px0, py1 - py0
        q = (px0 + ax0 * pw + r0, py0 + ay0 * ph + r1, px0 + ax1 * pw + r2, py0 + ay1 * ph + r3)
        quads[i] = q
        show = visible.get(i, not hidden)
        if parent >= 0 and parent in quads and not quads.get(('shown', parent), True):
            show = False
        quads[('shown', i)] = show
        if not show:
            continue
        x0, y0, x1, y1 = (round(v) for v in q)
        rgba = struct.unpack_from('<4f', row, 0x78)
        if kind == PANEL and rgba[3] > 0:
            layer = Image.new('RGBA', base.size, (0, 0, 0, 0))
            ImageDraw.Draw(layer).rectangle((x0, y0, x1 - 1, y1 - 1), fill=tuple(round(max(0, min(1, v)) * 255) for v in rgba))
            base.alpha_composite(layer)
        elif kind == SPRITE and x1 > x0 and y1 > y0:
            tex = TEXTURES.get(struct.unpack_from('<Q', row, 0x88)[0])
            if tex is None:
                continue
            u0, v0, u1, v1 = struct.unpack_from('<4f', row, 0x90)
            part = tex.crop((round(u0 * tex.width), round(v0 * tex.height), round(u1 * tex.width), round(v1 * tex.height)))
            base.alpha_composite(tinted(part.resize((x1 - x0, y1 - y0), Image.LANCZOS), rgba), (x0, y0))
        elif kind == TEXT:
            s = texts.get(i, lit.get(i, ''))
            if not s:
                continue
            size = struct.unpack_from('<H', row, 0xa4)[0]
            tx, ty = struct.unpack_from('<2f', row, 0x9c)
            anchor = ('l' if tx < .25 else 'r' if tx > .75 else 'm') + 'm'
            colour = tuple(round(max(0, min(1, v)) * 255) for v in rgba)
            d.text((q[0] + (q[2] - q[0]) * tx, q[1] + (q[3] - q[1]) * ty), s, font=font(size), fill=colour, anchor=anchor)


root = Canvas((BUILD / 'root.canvas').read_bytes())
nav = Canvas((BUILD / 'nav.canvas').read_bytes())
page = Canvas((BUILD / 'page.canvas').read_bytes())

STATES = {
    'idle': dict(page={}, texts={E['status']: 'TAP FIND MATCH TO QUEUE', E['stats_text']: '57 MINS', E['stats_caption']: 'HOURS TODAY'}),
    'searching': dict(page={E['card']: False, E['card_active']: True, E['timer']: True, E['timer_caption']: True,
                            E['find']: False, E['find_text']: False, E['cancel']: True, E['cancel_text']: True,
                            E['lobby']: False, E['lobby_text']: False, E['lobby_busy']: True,
                            E['private']: False, E['private_text']: False, E['private_busy']: True},
                      texts={E['status']: 'SEARCHING FOR A MATCH', E['timer']: '1:23', E['timer_caption']: 'IN QUEUE',
                             E['stats_text']: '108', E['stats_caption']: 'MATCHES OVERALL'}),
    'found': dict(page={E['card']: False, E['card_active']: True, E['timer']: True, E['timer_caption']: True,
                        E['find']: False, E['find_text']: False, E['cancel']: True, E['cancel_text']: True,
                        E['lobby']: False, E['lobby_text']: False, E['lobby_busy']: True,
                            E['private']: False, E['private_text']: False, E['private_busy']: True},
                  texts={E['status']: 'MATCH FOUND - JOINING IN 4...', E['timer']: '1:41', E['timer_caption']: 'MATCH FOUND',
                         E['stats_text']: '77', E['stats_caption']: 'PLAYERS ONLINE'}),
}

out = Path(a.out)
out.mkdir(parents=True, exist_ok=True)
shots = []
for name, st in STATES.items():
    img = Image.new('RGBA', (1024, 768), (0, 0, 0, 255))
    u0, v0, u1, v1 = SCREEN_UV
    screen = atlas.crop((round(u0 * atlas.width), round(v0 * atlas.height), round(u1 * atlas.width), round(v1 * atlas.height)))
    img.alpha_composite(screen.resize((1024, 768), Image.LANCZOS))
    # Tab bar with PLAY selected (stock selected sprites hidden), root title tab + page.
    nav_vis = {meta['nav_selected']: True, meta['nav_icon']: False}
    for i in (2, 4, 6, 8):
        nav_vis[i] = False
    draw_canvas(img, nav, 0, 0, visible=nav_vis)
    draw_canvas(img, root, 0, 0, visible={meta['root_child']: False, meta['root_cover']: True,
                                          meta['root_header']: True, meta['root_title']: True,
                                          meta.get('root_subtitle', -1): True})
    draw_canvas(img, page, PAGE_X, PAGE_Y, visible=st['page'], texts=st['texts'])
    path = out / f'play_{name}.png'
    img.convert('RGB').save(path)
    shots.append(img)
    print('wrote', path)
sheet = Image.new('RGB', (1024 * 3 + 40, 768), (20, 22, 26))
for k, s in enumerate(shots):
    sheet.paste(s.convert('RGB'), (k * (1024 + 20), 0))
sheet.save(out / 'play_states.png')
print('wrote', out / 'play_states.png')
