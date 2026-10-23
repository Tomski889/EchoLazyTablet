"""Build the PLAY tab for Echo VR's hand tablet from the installed game's own data.

Offline: reads the game's manifest/packages, writes build/tab/{manifests,packages},
the built canvases and tab icon (for tools/preview_tab.py) and src/generated/queue_tab.h.
Never modifies the game folder (see install_tab.py).

Same approach as Echo Arcade's ARCADE tab (kotorvr/echo-arcade, built on
heisthecat31/Doom-on-EchoVR's tablet tools). Everything is drawn from the game's own
tablet art: the tab icon is the stock tab frame with Echo's join glyph, the page uses the
stock card frames, button bar, icon tiles and text colours.

    python build_queue_tab.py --game "<...>\\ready-at-dawn-echo-arena" --tools "<Doom-on-EchoVR>\\EchoVr-Tablet-Probing"
"""
import argparse
import hashlib
import json
import os
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
OUT = ROOT / 'build/tab'
GENERATED = ROOT / 'src/generated'

args = argparse.ArgumentParser()
args.add_argument('--game', required=True, help='the ready-at-dawn-echo-arena folder that holds bin and _data')
args.add_argument('--tools', required=True, help="Doom-on-EchoVR's EchoVr-Tablet-Probing folder")
args.add_argument('--data', help='a data folder (manifests/ + packages/) to build from instead of the game\'s, e.g. '
                                 'the pre-EchoQueue manifest beside a link to the game\'s packages, for previews')
args = args.parse_args()
os.environ['ECHOVR_GAME'] = args.game
if args.data:
    os.environ['ECHOVR_DATA'] = args.data
sys.path.insert(0, args.tools)

import echovr_pkg as P  # noqa: E402
from canvas_edit import Canvas, descriptor, u64  # noqa: E402
from echovr_patch import Patcher, ManifestFile, MID, GAME_MANIFEST, GAME_PACKAGES, verify  # noqa: E402
from build_tools_tab import empty_canvas, ROOT_CANVAS, NAV_CANVAS, LEVEL, ACTOR  # noqa: E402

TITLE_DONOR = 0xfdea8aeb3a0f4862          # stock canvas: element 0 panel, element 1 text
NAV_SOURCE_BUTTON = 0x275876572b742791    # first stock tab button; ours are cloned from it
STOCK_TAB_FIRST, STOCK_TAB_LAST = 0x275876572b742791, 0x275876572b742794
PAGE = P.sym('echo_queue_page_v1')
ICON_TEXTURE = P.sym('echo_queue_tab_icon_texture_v1')
TAB_BUTTON = P.sym('echo_queue_tab_button_v1')
START_BUTTON = P.sym('echo_queue_start_button_v1')
CANCEL_BUTTON = P.sym('echo_queue_cancel_button_v1')
LOBBY_BUTTON = P.sym('echo_queue_lobby_button_v1')
STATS_BUTTON = P.sym('echo_queue_stats_button_v1')
PRIVATE_BUTTON = P.sym('echo_queue_private_button_v1')

# Geometry in tablet canvas pixels (1024 px = 0.3 m), as Echo Arcade uses it.
# The stock pages are 942 px wide at x 28 (13 px left of centre). Ours is wider and centred on
# the tablet, so the card's gap at the sides matches its gap above and below.
PAGE_X, PAGE_Y, PAGE_W, PAGE_H = 12, 112, 1000, 528
DX = (PAGE_W - 942) // 2                                # content below is laid out on 942 px, centred
TAB_Y = (662, 751)
TAB_SIZE = 89                                           # stock tab icon size
TAB_SPACING = 170                                       # 5 tabs between the bar's corner marks (~61..929)
TAB_CENTERS = tuple(512 + (i - 2) * TAB_SPACING for i in range(5))
QUEUE_SLOT, STOCK_SLOTS = 0, (1, 2, 3, 4)

# Page layout.
# Echo's tablet font is wider than it looks in previews: leave room around all text.
CARD = (2, 8, PAGE_W - 2, 520)                          # one card round everything (teal while searching)
MODE_TILE = (44 + DX, 30, 164 + DX, 150)
BUTTON = (64 + DX, 198, 878 + DX, 284)                  # FIND MATCH / CANCEL SEARCH (same spot)
COLUMNS = (171 + DX, 471 + DX, 771 + DX)                # bottom row: AUTO LOBBY, LAST WAIT, MATCHES
ROW_CAPTION, ROW_BOX, ROW_FOOT = (318, 350), (358, 468), (474, 512)

# The game's own tablet art (UI atlas 2048 x 3424), measured off the atlas.
ATLAS = 0x79563b60c7895d4e
CARD_UV = (0.2520, 0.4632, 0.4927, 0.5686)              # dark card, white corner marks
CARD_ACTIVE_UV = (0.0142, 0.4632, 0.2427, 0.5634)       # same card, teal border (selected)
BUTTON_UV = (0.4883, 0.6259, 0.9849, 0.6714)            # white rounded bar, tinted
TILE_UV = (0.831, 0.101, 0.961, 0.178)                  # teal-outlined icon tile
FRAME_UV, FRAME_INNER_UV = (0.88, 0.682, 0.987, 0.746), (0.518, 0.536, 0.615, 0.594)  # orange selected frame
JOIN_ICON_UV = (0.5991, 0.0079, 0.6709, 0.0724)         # faces + down arrow (139 x 213 px)
REFRESH_ICON_UV = (0.4707, 0.0041, 0.564, 0.0599)       # circular arrows (183 x 183 px)
SCREEN_UV = (0.015, 0.232, 0.515, 0.456)                # the tablet screen background, 1024 x 768
# The lobby list's player rows (stock canvas 5e7f84bb4c40e9e0), drawn from a second UI atlas:
# a background, a tinted fill, and a border in eight 32 px pieces so it keeps its shape.
ROW_ATLAS = 0x9385350a879a938b
ROW_BACK_UV, ROW_FILL_UV = (0.0215, 0.4609, 0.0801, 0.5215), (0.0078, 0.0059, 0.0117, 0.5039)
ROW_BORDER_UV = dict(tl=(0.793, 0.0059, 0.8535, 0.0664), t=(0.4688, 0.0059, 0.5293, 0.0664), tr=(0.5312, 0.0059, 0.5918, 0.0664),
                     l=(0.4062, 0.0684, 0.4668, 0.1289), r=(0.5312, 0.0684, 0.5918, 0.1289),
                     bl=(0.793, 0.1309, 0.8535, 0.1914), b=(0.4688, 0.1309, 0.5293, 0.1914), br=(0.5312, 0.1309, 0.5918, 0.1914))
ROW_FILL, ROW_BORDER = (.37, .42, .49, 1), (.42, .48, .55, 1)                   # the stock row's colours
FIND_FILL, FIND_BORDER = (.05, .46, .45, 1), (.2, .78, .75, 1)                  # Echo's teal, as the main action
CANCEL_FILL, CANCEL_BORDER = (.5, .24, .22, 1), (.66, .32, .29, 1)
TAB_IDLE_UV = (0.6904, 0.3645, 0.7393, 0.3937)          # stock List tab frame, grey (idle)
TAB_SELECTED_UV = (0.6899, 0.3297, 0.7393, 0.3595)      # and teal (selected)
BGRA_TEMPLATE = 0x84b9a06506af66fe                      # stock 128x128 B8G8R8A8_UNORM texture
DXGI_B8G8R8A8_UNORM, DXGI_B8G8R8A8_UNORM_SRGB = 87, 91

TEAL, CANCEL_RED = (.05, .55, .54, 1), (.78, .26, .22, 1)
ORANGE, ORANGE_INNER = (.97, .6, .13, 1), (.99, .87, .47, 1)
OFF_RED = (.98, .45, .33, 1)
TEXT, WHITE, BLACK, CLEAR = (.78, .78, .78, 1), (1, 1, 1, 1), (0, 0, 0, 1), (0, 0, 0, 0)
DIM, ACCENT = (.5, .54, .6, 1), (.2, .85, .82, 1)


def rect_of(row):
    return list(struct.unpack_from('<4f', row, 0x34))


def respace_stock_tabs(nav):
    """Move the 4 stock tab icons (idle + selected sprite pairs) to slots 1..4 (Echo Arcade's method).

    Returns [(old_center, new_center, idle_index, selected_index)] left to right."""
    moves = []
    for k, (idle, sel) in enumerate([(1, 2), (3, 4), (5, 6), (7, 8)]):
        a, b = rect_of(nav.elements[idle]), rect_of(nav.elements[sel])
        old = (a[0] + a[2]) / 2
        uv_idle, uv_sel = struct.unpack_from('<4f', nav.elements[idle], 0x90), struct.unpack_from('<4f', nav.elements[sel], 0x90)
        if abs(old - 204.8 * (k + 1)) >= 2 or abs((b[0] + b[2]) / 2 - old) >= 2 or uv_sel[1] >= uv_idle[1]:
            raise SystemExit('Unexpected stock tab layout (another tablet mod installed?). Restore it first.')
        new = TAB_CENTERS[STOCK_SLOTS[k]]
        for i in (idle, sel):
            r = rect_of(nav.elements[i])
            struct.pack_into('<4f', nav.elements[i], 0x34, r[0] + new - old, r[1], r[2] + new - old, r[3])
        moves.append((old, new, idle, sel))
    return moves


def colour(canvas, index, rgba):
    struct.pack_into('<4f', canvas.elements[index], 0x78, *rgba)


def fix_texture_count(canvas):
    """Header +0x28 = elements that reference a texture (sprite 3, mask 4, colour mask 8); see Echo Arcade."""
    count = sum(struct.unpack_from('<I', e, 8)[0] in (3, 4, 8) for e in canvas.elements)
    struct.pack_into('<I', canvas.header, 0x28, count)


def fix_budget(canvas):
    """Header +0x2c/+0x30: vertex/index budget. Stock canvases reserve one quad per panel or
    sprite and one per text character of capacity (4 vertices, 6 indices each)."""
    quads = 0
    for e in canvas.elements:
        kind = struct.unpack_from('<I', e, 8)[0]
        if kind in (2, 3):
            quads += 1
        elif kind == 1:
            quads += struct.unpack_from('<II', e, 0x90)[1]
    struct.pack_into('<2I', canvas.header, 0x2c, 4 * quads, 6 * quads)


# ---- tab icon texture ----
def atlas_image(stock, name=ATLAS):
    import texture2ddecoder
    from PIL import Image
    tt, tg = P.typesym('CGTextureResource'), P.typesym('CGTextureResource', True)
    cpu, gpu = stock.get(tt, name), stock.get(tg, name)
    w, h = struct.unpack_from('<II', cpu, 0xc4)
    if struct.unpack_from('<I', gpu, 128)[0] != 99:
        raise SystemExit('Unexpected tablet atlas format (game update?)')
    return Image.frombytes('RGBA', (w, h), texture2ddecoder.decode_bc7(gpu[148:148 + (w // 4) * (h // 4) * 16], w, h), 'raw', 'BGRA')


def crop_uv(atlas, uv):
    u0, v0, u1, v1 = uv
    return atlas.crop((round(u0 * atlas.width), round(v0 * atlas.height), round(u1 * atlas.width), round(v1 * atlas.height)))


def tab_frame(atlas, uv):
    """A stock tab frame with its List glyph painted out: inside the border, every bright
    pixel (glyph) and its edge is replaced by that row's typical interior colour."""
    from PIL import Image, ImageFilter
    img = crop_uv(atlas, uv).convert('RGBA')
    w, h = img.size
    px = img.load()
    inset = round(w * .15)
    mask = Image.new('L', img.size, 0)
    mk = mask.load()
    for y in range(inset, h - inset):
        for x in range(inset, w - inset):
            if min(px[x, y][:3]) > 150:
                mk[x, y] = 255
    mk = mask.filter(ImageFilter.MaxFilter(5)).load()
    for y in range(inset, h - inset):
        clean = sorted((px[x, y] for x in range(inset, w - inset) if not mk[x, y]), key=lambda p: sum(p[:3]))
        if not clean:
            continue
        fill = clean[len(clean) // 2]
        for x in range(inset, w - inset):
            if mk[x, y]:
                px[x, y] = fill
    return img


def screen_uv(rect):
    """UVs of the tablet screen background under `rect` (tablet canvas pixels), so a sprite
    drawn there looks like the bare screen: it covers stock content without hiding it."""
    u0, v0, u1, v1 = SCREEN_UV
    x0, y0, x1, y1 = rect
    return (u0 + x0 / 1024 * (u1 - u0), v0 + y0 / 768 * (v1 - v0), u0 + x1 / 1024 * (u1 - u0), v0 + y1 / 768 * (v1 - v0))


def icon_texture(atlas):
    """-> (w, h, BGRA bytes, uvs, preview image). 256 x 256: the PLAY tab (stock tab frames, idle
    and selected, with a disc), then the disc glyph and the lobby (house) glyph for the page's
    tiles. Glyphs: tools/disc_icon.py."""
    from PIL import Image
    from disc_icon import disc, house
    tex = Image.new('RGBA', (256, 256), (0, 0, 0, 0))
    uvs = {}
    for col, (key, uv) in enumerate((('tab_idle', TAB_IDLE_UV), ('tab_selected', TAB_SELECTED_UV))):
        frame = tab_frame(atlas, uv)
        w, h = frame.size
        g = disc(round(h * .66))
        frame.alpha_composite(g, ((w - g.width) // 2, (h - g.height) // 2))
        tex.paste(frame, (col * 128, 0))
        uvs[key] = (col * 128 / 256, 0, (col * 128 + w) / 256, h / 256)
    tex.alpha_composite(disc(120), (4, 132))
    uvs['disc'] = (4 / 256, 132 / 256, 124 / 256, 252 / 256)
    tex.alpha_composite(house(120), (132, 132))
    uvs['house'] = (132 / 256, 132 / 256, 252 / 256, 252 / 256)
    r, g, b, a = tex.split()
    return 256, 256, Image.merge('RGBA', (b, g, r, a)).tobytes(), uvs, tex


def texture_pair(stock, w, h, pixels, fmt=DXGI_B8G8R8A8_UNORM_SRGB):
    """CGTextureResource Win10 + Win10GPU pair: the stock BGRA texture's headers resized to w x h."""
    tt, tg = P.typesym('CGTextureResource'), P.typesym('CGTextureResource', True)
    cpu = bytearray(stock.get(tt, BGRA_TEMPLATE))
    gpu = stock.get(tg, BGRA_TEMPLATE)
    if len(cpu) != 256 or struct.unpack_from('<I', cpu, 0xd8)[0] != DXGI_B8G8R8A8_UNORM:
        raise SystemExit('Unexpected BGRA template texture')
    dds = bytearray(gpu[:148])
    if dds[:4] != b'DDS ' or dds[84:88] != b'DX10' or struct.unpack_from('<I', dds, 128)[0] != DXGI_B8G8R8A8_UNORM:
        raise SystemExit('Unexpected BGRA template DDS header')
    pitch = w * 4
    struct.pack_into('<III', dds, 12, h, w, pitch)
    struct.pack_into('<I', dds, 128, fmt)
    struct.pack_into('<II', cpu, 0xc4, w, h)
    struct.pack_into('<I', cpu, 0xd8, fmt)
    struct.pack_into('<II', cpu, 0xe8, w, h)
    struct.pack_into('<II', cpu, 0xf4, len(dds) + w * h * 4, pitch)
    assert len(pixels) == w * h * 4
    return bytes(cpu), bytes(dds) + pixels


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    patcher = Patcher()
    stock = patcher.stock
    base_manifest = GAME_MANIFEST.read_bytes()
    before = ManifestFile(base_manifest)
    cv = P.typesym('CUICanvasResource')
    if stock.get(cv, PAGE) is not None:
        raise SystemExit('The PLAY tab is already installed in this manifest; restore it first (install_tab.py restore).')
    root = Canvas(stock.get(cv, ROOT_CANVAS))
    nav = Canvas(stock.get(cv, NAV_CANVAS))
    donor = Canvas(stock.get(cv, TITLE_DONOR))
    if len(root.elements) != 7 or len(nav.elements) != 9:
        raise SystemExit('Unexpected stock tablet layout (another tablet mod installed?). Restore it first.')
    old_root = [bytes(r) for r in root.elements]
    old_nav = [bytes(r) for r in nav.elements]
    text_template, panel_template = donor.elements[1], donor.elements[0]
    sprite_template = nav.elements[0]
    assert struct.unpack_from('<I', sprite_template, 8)[0] == 3

    # Navigation: PLAY icon tab in slot 0 (idle + selected, like the stock tabs), stock tabs in 1..4.
    moves = respace_stock_tabs(nav)
    atlas = atlas_image(stock)
    sys.path.insert(0, str(HERE))
    icon_w, icon_h, icon_pixels, icon_uvs, icon_preview = icon_texture(atlas)
    c = TAB_CENTERS[QUEUE_SLOT]
    tab_rect = (c - TAB_SIZE / 2, TAB_Y[0], c + TAB_SIZE / 2, TAB_Y[1])

    def tab_sprite(template, name, uv, hidden):
        i = nav.append(template, name, tab_rect, parent=struct.unpack_from('<i', template, 0x5c)[0], hidden=hidden)
        row = nav.elements[i]
        row[0x24:0x34] = template[0x24:0x34]
        colour(nav, i, WHITE)
        struct.pack_into('<Q', row, 0x88, ICON_TEXTURE)
        struct.pack_into('<4f', row, 0x90, *uv)
        return i

    nav_icon = tab_sprite(nav.elements[3], 'echo_queue_tab_icon_v1', icon_uvs['tab_idle'], False)
    nav_selected = tab_sprite(nav.elements[4], 'echo_queue_tab_selected_v1', icon_uvs['tab_selected'], True)

    # Page.
    page = empty_canvas(stock.get(cv, TITLE_DONOR), PAGE_W, PAGE_H)
    struct.pack_into('<Q', page.header, 0, PAGE)

    def sprite(name, rect, uv, rgba, parent=-1, hidden=False, texture=ATLAS, canvas=None):
        canvas = canvas or page
        i = canvas.append(sprite_template, name, rect, parent=parent, hidden=hidden)
        row = canvas.elements[i]
        row[0x24:0x34] = sprite_template[0x24:0x34]
        colour(canvas, i, rgba)
        struct.pack_into('<Q', row, 0x88, texture)
        struct.pack_into('<4f', row, 0x90, *uv)
        return i

    def text(name, words, rect, size, rgba, align=.5, hidden=False, capacity=0):
        i = page.label(text_template, name, words, rect, size, hidden=hidden, capacity=capacity)
        colour(page, i, rgba)
        struct.pack_into('<2f', page.elements[i], 0x9c, align, .5)
        return i

    def selected_frame(name, rect):
        """Echo's orange 'selected' frame (outer + inner), hidden until the runtime shows it."""
        x0, y0, x1, y1 = rect
        outer = sprite(name, (x0 - 6, y0 - 6, x1 + 6, y1 + 6), FRAME_UV, ORANGE, hidden=True)
        sprite(name + '_inner', (0, 0, x1 - x0 + 12, y1 - y0 + 12), FRAME_INNER_UV, ORANGE_INNER, parent=outer)
        return outer

    # The bare tablet screen over the whole page: the stock page underneath is covered, so the
    # runtime never has to hide (and keep re-hiding) the game's own elements.
    background = sprite('echo_queue_background_v1', (0, 0, PAGE_W, PAGE_H),
                        screen_uv((PAGE_X, PAGE_Y, PAGE_X + PAGE_W, PAGE_Y + PAGE_H)), WHITE)

    # Match card: mode tile, name, mode, status; teal border and a queue timer while searching.
    e = {}
    e['card'] = sprite('echo_queue_card_v1', CARD, CARD_UV, WHITE)
    e['card_active'] = sprite('echo_queue_card_active_v1', CARD, CARD_ACTIVE_UV, WHITE, hidden=True)
    tile = sprite('echo_queue_mode_tile_v1', MODE_TILE, TILE_UV, TEAL)
    tw = MODE_TILE[2] - MODE_TILE[0]
    sprite('echo_queue_mode_icon_v1', (16, 16, tw - 16, tw - 16), icon_uvs['disc'], WHITE, parent=tile, texture=ICON_TEXTURE)
    text('echo_queue_title_text_v1', 'ECHO ARENA', (190 + DX, 26, 620 + DX, 80), 52, WHITE, align=0)
    text('echo_queue_mode_text_v1', 'PUBLIC MATCH', (192 + DX, 80, 620 + DX, 110), 24, ACCENT, align=0)
    e['status'] = text('echo_queue_status_v1', 'TAP FIND MATCH TO QUEUE', (192 + DX, 112, 640 + DX, 146), 24, TEXT, align=0, capacity=96)
    # The state, big on the right: READY while idle, the queue timer while searching.
    e['timer'] = text('echo_queue_timer_v1', 'READY', (620 + DX, 30, 896 + DX, 112), 64, WHITE, align=1, capacity=16)
    e['timer_caption'] = text('echo_queue_timer_caption_v1', 'STATUS', (620 + DX, 112, 896 + DX, 142), 20, ACCENT, align=1, capacity=24)

    def lobby_row(name, rect, fill, border, title, hidden=False):
        """A lobby-list row (no icon tile): its background, fill and 8-piece border, with its
        title centred. Everything is a child of the background, so showing/hiding that one
        element shows/hides the row. -> (row, title)."""
        x0, y0, x1, y1 = rect
        w, h = x1 - x0, y1 - y0
        row = sprite(name, rect, ROW_BACK_UV, WHITE, hidden=hidden, texture=ROW_ATLAS)
        sprite(name + '_fill', (0, 0, w, h), ROW_FILL_UV, fill, parent=row, texture=ROW_ATLAS)
        k = 32
        pieces = dict(tl=(0, 0, k, k), t=(k, 0, w - k, k), tr=(w - k, 0, w, k), l=(0, k, k, h - k), r=(w - k, k, w, h - k),
                      bl=(0, h - k, k, h), b=(k, h - k, w - k, h), br=(w - k, h - k, w, h))
        for piece, r in pieces.items():
            sprite(f'{name}_{piece}', r, ROW_BORDER_UV[piece], border, parent=row, texture=ROW_ATLAS)
        label = text(name + '_title', title, (24, 0, w - 24, h), 46, WHITE)
        struct.pack_into('<i', page.elements[label], 0x5c, row)
        return row, label

    # FIND MATCH, swapped for CANCEL SEARCH while searching: lobby-list rows.
    e['find'], e['find_text'] = lobby_row('echo_queue_find_row_v1', BUTTON, FIND_FILL, FIND_BORDER, 'FIND MATCH')
    e['cancel'], e['cancel_text'] = lobby_row('echo_queue_cancel_row_v1', BUTTON, CANCEL_FILL, CANCEL_BORDER,
                                              'CANCEL SEARCH', hidden=True)

    # Bottom row: three matching boxes, caption above. LOBBY and PRIVATE MATCH are buttons,
    # teal like FIND MATCH; while matchmaking they turn into plain boxes saying BUSY.
    def column(cx, caption, capacity=0):
        return text(f'echo_queue_caption_{cx}_v1', caption, (cx - 140, ROW_CAPTION[0], cx + 140, ROW_CAPTION[1]), 22, WHITE, capacity=capacity)

    def box_at(cx):
        return (cx - 96, ROW_BOX[0] + 8, cx + 96, ROW_BOX[1] + 8)

    def action_box(key, cx, caption, label, icon=None, busy=True, capacity=0):
        """-> the box's rect. e[key] (button look, with its fill and icon as children),
        e[key_text] (label) and e[key_busy] (BUSY, hidden) unless busy=False."""
        e[key + '_caption'] = column(cx, caption, capacity)
        rect = box_at(cx)
        w, h = rect[2] - rect[0], rect[3] - rect[1]
        sprite(f'echo_queue_{key}_base_v1', rect, CARD_UV, WHITE)  # the plain box, under BUSY
        e[key] = sprite(f'echo_queue_{key}_box_v1', rect, CARD_ACTIVE_UV, WHITE)
        sprite(f'echo_queue_{key}_fill_v1', (6, 6, w - 6, h - 6), ROW_FILL_UV, (*FIND_FILL[:3], .55), parent=e[key], texture=ROW_ATLAS)
        label_rect = rect
        if icon:
            side = 64
            sprite(f'echo_queue_{key}_icon_v1', (30, (h - side) / 2, 30 + side, (h + side) / 2), icon_uvs[icon], WHITE,
                   parent=e[key], texture=ICON_TEXTURE)
            label_rect = (rect[0] + 30 + side, rect[1], rect[2] - 10, rect[3])
        e[key + '_text'] = text(f'echo_queue_{key}_text_v1', label, label_rect, 40, WHITE, capacity=capacity)
        if busy:
            e[key + '_busy'] = text(f'echo_queue_{key}_busy_v1', 'BUSY', rect, 40, DIM, hidden=True)
        return rect

    private_rect = action_box('private', COLUMNS[0], 'PRIVATE MATCH', 'CREATE')
    lobby_rect = action_box('lobby', COLUMNS[1], 'LOBBY', 'GO', icon='house')
    # Tapping it steps through HOURS TODAY, MATCHES OVERALL and PLAYERS ONLINE (caption and
    # number both set by the runtime).
    stats_rect = action_box('stats', COLUMNS[2], 'HOURS TODAY', '0 MINS', busy=False, capacity=16)

    # Root: page child + title tab, hidden until PLAY is selected. The title tab copies the
    # stock LOBBY one: white tab, black text.
    child = root.append(root.elements[1], 'echo_queue_page_child_v1', (PAGE_X, PAGE_Y, PAGE_X + PAGE_W, PAGE_Y + PAGE_H), hidden=True)
    struct.pack_into('<Q', root.elements[child], 0x78, PAGE)
    # The bare screen over the stock title (covers it, so it never has to be hidden).
    title_area = (54, 22, 822, 103)  # stops above the divider line under the title
    cover = sprite('echo_queue_title_cover_v1', title_area, screen_uv(title_area), WHITE, hidden=True, canvas=root)
    header = root.append(panel_template, 'echo_queue_header_background_v1', (60, 40, 210, 108), hidden=True)
    colour(root, header, WHITE)
    title = root.label(text_template, 'echo_queue_title_v1', 'PLAY', (76, 40, 210, 108), 44, hidden=True, capacity=64)
    colour(root, title, BLACK)
    struct.pack_into('<2f', root.elements[title], 0x9c, 0, .5)
    # Beside the tab, so the strip the cover leaves isn't an empty bar.
    subtitle = root.label(text_template, 'echo_queue_subtitle_v1', 'ARENA MATCHMAKING', (236, 40, 800, 108), 30, hidden=True, capacity=64)
    colour(root, subtitle, DIM)
    struct.pack_into('<2f', root.elements[subtitle], 0x9c, 0, .5)

    for canvas in (root, nav, page):
        fix_texture_count(canvas)
        fix_budget(canvas)
        canvas.validate()
    assert struct.unpack_from('<I', nav.header, 0x28)[0] == 11
    assert [bytes(r) for r in root.elements[:7]] == old_root
    for i, (old, new) in enumerate(zip(old_nav, nav.elements[:9])):  # stock tabs: only their x position moved
        assert old[:0x34] == new[:0x34] and old[0x44:] == new[0x44:], f'nav element {i} changed beyond its rect'

    # Native touch buttons, cloned from a stock tab button.
    bt = P.typesym('CR15ButtonInteractCR')
    original = stock.get(bt, LEVEL)
    stride, rows = P.parse_cr(original)
    if stride != 296:
        raise SystemExit('Unexpected button record size')
    source = next(r for r in rows if u64(r, 8) == ACTOR and u64(r, 0) == NAV_SOURCE_BUTTON)
    s = .3 / 1024

    def add_button(name_sym, rect, on_page=True):
        row = bytearray(source)
        struct.pack_into('<Q', row, 0, name_sym)
        x0, y0, x1, y1 = rect
        if on_page:
            x0, y0, x1, y1 = x0 + PAGE_X, y0 + PAGE_Y, x1 + PAGE_X, y1 + PAGE_Y
        struct.pack_into('<2f', row, 0x80, (x0 + x1) / 2 * s, -(y0 + y1) / 2 * s)
        struct.pack_into('<2f', row, 0x9c, (x1 - x0) / 2 * s, (y1 - y0) / 2 * s)
        rows.append(bytes(row))

    moved = set()
    for i, r in enumerate(rows):  # stock tab hitboxes follow their icons
        if u64(r, 8) != ACTOR or not STOCK_TAB_FIRST <= u64(r, 0) <= STOCK_TAB_LAST:
            continue
        x, _ = struct.unpack_from('<2f', r, 0x80)
        old, new, _, _ = min(moves, key=lambda m: abs(m[0] - x / s))
        if abs(old - x / s) >= 3:
            raise SystemExit(f'Stock tab hitbox not under a stock icon ({x / s:.1f})')
        row = bytearray(r)
        struct.pack_into('<f', row, 0x80, new * s)
        rows[i] = bytes(row)
        moved.add(new)
    if len(moved) != 4:
        raise SystemExit('Expected 4 stock tab hitboxes')
    changed_rows = {i for i, r in enumerate(rows) if STOCK_TAB_FIRST <= u64(r, 0) <= STOCK_TAB_LAST and u64(r, 8) == ACTOR}
    add_button(TAB_BUTTON, tab_rect, on_page=False)
    add_button(START_BUTTON, BUTTON)
    add_button(CANCEL_BUTTON, BUTTON)
    add_button(LOBBY_BUTTON, lobby_rect)
    add_button(STATS_BUTTON, stats_rect)
    add_button(PRIVATE_BUTTON, private_rect)
    hdr = bytearray(original[:56])
    descriptor(hdr, 0, len(rows), stride)
    buttons = bytes(hdr) + b''.join(rows)
    _, original_rows = P.parse_cr(original)
    for i, r in enumerate(original_rows):
        assert i in changed_rows or rows[i] == r, 'Unrelated button record changed'

    icon_cpu, icon_gpu = texture_pair(stock, icon_w, icon_h, icon_pixels)
    patcher.add_item('CUICanvasResource', False, f'0x{ROOT_CANVAS:016x}', root.serialize())
    patcher.add_item('CUICanvasResource', False, f'0x{NAV_CANVAS:016x}', nav.serialize())
    patcher.add_item('CUICanvasResource', False, f'0x{PAGE:016x}', page.serialize())
    patcher.add_item('CR15ButtonInteractCR', False, f'0x{LEVEL:016x}', buttons)
    patcher.add_item('CGTextureResource', False, f'0x{ICON_TEXTURE:016x}', icon_cpu)
    patcher.add_item('CGTextureResource', True, f'0x{ICON_TEXTURE:016x}', icon_gpu)
    # Package files other tools left behind are kept as empty packages, never overwritten.
    while (GAME_PACKAGES / f'{MID}_{patcher.mf.npkg}').exists():
        i = patcher.mf.npkg
        patcher.mf.C.append([i, (GAME_PACKAGES / f'{MID}_{i}').stat().st_size, 0, 0])
        patcher.mf.npkg += 1
    package_index = patcher.mf.npkg
    patcher.build(OUT)

    after = ManifestFile((OUT / 'manifests' / MID).read_bytes())
    overrides = {(t, n) for t, n, _ in patcher.items}
    after_rows = {(a[0], a[1]): (a, b) for a, b in zip(after.A, after.B)}
    for a, b in zip(before.A, before.B):
        if (a[0], a[1]) not in overrides:
            assert after_rows[(a[0], a[1])] == (a, b), 'Unrelated resource changed'
    assert after.C[:len(before.C)] == before.C, 'Existing frame references changed'
    if not verify(OUT):
        raise SystemExit('Package verification failed')

    # For tools/preview_tab.py.
    (OUT / 'root.canvas').write_bytes(root.serialize())
    (OUT / 'nav.canvas').write_bytes(nav.serialize())
    (OUT / 'page.canvas').write_bytes(page.serialize())
    icon_preview.save(OUT / 'tab_icon.png')
    atlas.save(OUT / 'atlas.png')
    atlas_image(stock, ROW_ATLAS).save(OUT / 'row_atlas.png')

    sha = lambda b: hashlib.sha256(b).hexdigest()
    package_name = f'{MID}_{package_index}'
    meta = dict(schema=2, base_manifest_sha256=sha(base_manifest),
                manifest_sha256=sha((OUT / 'manifests' / MID).read_bytes()),
                package=package_name, package_sha256=sha((OUT / 'packages' / package_name).read_bytes()),
                page=f'{PAGE:016x}', icon_texture=f'{ICON_TEXTURE:016x}', atlas=f'{ATLAS:016x}', row_atlas=f'{ROW_ATLAS:016x}',
                elements={k: v for k, v in e.items()}, root_child=child, root_cover=cover, root_header=header, root_title=title,
                root_subtitle=subtitle,
                nav_icon=nav_icon, nav_selected=nav_selected)
    (OUT / 'queue_tab.json').write_text(json.dumps(meta, indent=2))

    GENERATED.mkdir(parents=True, exist_ok=True)
    (GENERATED / 'queue_tab.h').write_text(
        '// Generated by tools/build_queue_tab.py -- do not edit.\n#pragma once\n#include <cstdint>\n'
        f'constexpr uint64_t QUEUE_TAB=0x{TAB_BUTTON:016x}, QUEUE_START=0x{START_BUTTON:016x}, '
        f'QUEUE_CANCEL=0x{CANCEL_BUTTON:016x}, QUEUE_LOBBY=0x{LOBBY_BUTTON:016x}, QUEUE_PRIVATE=0x{PRIVATE_BUTTON:016x}, '
        f'QUEUE_STATS=0x{STATS_BUTTON:016x};\n'
        f'constexpr uint64_t QUEUE_ROOT_MARKER=0x{P.sym("echo_queue_page_child_v1"):016x}, '
        f'QUEUE_NAV_MARKER=0x{P.sym("echo_queue_tab_icon_v1"):016x}, QUEUE_PAGE_MARKER=0x{P.sym("echo_queue_background_v1"):016x};\n'
        f'constexpr unsigned QUEUE_ROOT_ELEMENTS={len(root.elements)}, QUEUE_NAV_ELEMENTS={len(nav.elements)}, QUEUE_PAGE_ELEMENTS={len(page.elements)};\n'
        f'constexpr unsigned QUEUE_ROOT_CHILD={child}, QUEUE_ROOT_COVER={cover}, QUEUE_ROOT_HEADER={header}, QUEUE_ROOT_TITLE={title}, '
        f'QUEUE_ROOT_SUBTITLE={subtitle};\n'
        f'constexpr unsigned QUEUE_NAV_ICON={nav_icon}, QUEUE_NAV_SELECTED={nav_selected};\n'
        f'constexpr unsigned QUEUE_NAV_STOCK_SELECTED[]={{{",".join(str(m[3]) for m in moves)}}};\n'
        + ''.join(f'constexpr unsigned QUEUE_PAGE_{k.upper()}={v};\n' for k, v in e.items()))
    print(f'Built the PLAY tab: {len(patcher.items)} resources in {package_name}. No game files changed.')
    return meta


if __name__ == '__main__':
    build()
