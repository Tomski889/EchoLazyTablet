"""Builds EchoLazyTablet's PLAY tab on top of the hand tablet as it is now, other tablet mods included.

build(manifest) takes a manifest with no EchoLazyTablet in it (the patcher takes ours out first) and
returns the resources to add, the layout file the plugin reads, and notes on what it found:

  * the root canvas, tab bar and touch-button table are whatever the manifest holds now (stock
    or another mod's). No canvas changes its element count (other mods' plugins find the tablet
    by exact counts, e.g. Echo Arcade's root 10 / tab bar 13): everything EchoLazyTablet shows is one
    overlay canvas of its own, shown by the root's unused stock element 6 (a hidden panel,
    turned into a child-canvas element). Touch areas are appended to the button table;
  * every tab in the tab bar (stock or another mod's: anything tab-sized in the bar strip, with
    the tab-sized touch area under it) is spread evenly with PLAY first, the touch areas moved
    with their icons;
  * the plugin finds its canvases by element count plus one element's name; for each canvas the
    name is picked so that no other canvas with that many elements has it at that index.

All art comes from the game's own tablet atlas (nothing from the game ships with EchoLazyTablet).
`python tablet.py --game <...> --header <out.h>` regenerates the plugin's page constants.
"""
import struct

import echodata as E
from echodata import Canvas, Records, sym, typesym, u64

CV = typesym('CUICanvasResource')
BUTTONS = typesym('CR15ButtonInteractCR')
TEX, TEX_GPU = typesym('CGTextureResource'), typesym('CGTextureResource', True)
LEVEL = sym('r14_glb_global_mp')
ACTOR = 0x6c1f6ff04e070923                # the hand tablet
ROOT_CANVAS, NAV_CANVAS = 0x30b4d30bbcb8d444, 0xfdea8ae53c32407e
TITLE_DONOR = 0xfdea8aeb3a0f4862          # stock canvas: element 0 panel, element 1 text
# The stock pages the plugin fades out under PLAY (lobby list, its rows, settings, title).
STOCK_PAGES = (0x879940b6a9cd27a5, 0x59d6fe101e307297, 0xcc12da433b3df4ac, 0xfdea8aeb3a0f4862)
STOCK_TABS = (0x275876572b742791, 0x275876572b742792, 0x275876572b742793, 0x275876572b742794)
STOCK_TAB_PAIRS = ((1, 2), (3, 4), (5, 6), (7, 8))   # (idle, selected) sprites in the stock nav
STOCK_BUTTON_ROWS = 41                    # touch areas in the stock r14_glb_global_mp table

PAGE = sym('echo_queue_page_v1')         # v3.8 and before: the PLAY page canvas (now inside OVERLAY)
OVERLAY = sym('echo_queue_overlay_v1')    # v3.9: everything EchoLazyTablet shows, on the root's element 6
ROOT_SLOT, ROOT_SLOT_NAME = 6, 0xfb8485552d32128c
ICON_TEXTURE = sym('echo_queue_tab_icon_texture_v1')
TAB_BUTTON = sym('echo_queue_tab_button_v1')
START_BUTTON = sym('echo_queue_start_button_v1')
CANCEL_BUTTON = sym('echo_queue_cancel_button_v1')
LOBBY_BUTTON = sym('echo_queue_lobby_button_v1')
STATS_BUTTON = sym('echo_queue_stats_button_v1')
PRIVATE_BUTTON = sym('echo_queue_private_button_v1')
GFX_HIGH_BUTTON = sym('echo_queue_gfx_high_button_v1')
GFX_LOW_BUTTON = sym('echo_queue_gfx_low_button_v1')
CLIP_BUTTON = sym('echo_queue_clip_button_v1')
CLIP_KEY_BUTTON = sym('echo_queue_clip_key_button_v1')    # opens the CLIP HOTKEY page
CLOCK_BUTTON = sym('echo_queue_clock_format_button_v1')  # the title strip's 12H / 24H switch

# The CLIP HOTKEY page's keyboard: (label, what the plugin does with it). Its touch areas are
# echo_queue_key_<label>_v1.
KEYBOARD_ROWS = (['F1', 'F2', 'F3', 'F4', 'F5', 'F6', 'F7', 'F8', 'F9', 'F10', 'F11', 'F12'],
                 list('1234567890'), list('QWERTYUIOP'), list('ASDFGHJKL'), list('ZXCVBNM'))
KEYBOARD_EXTRA = ('CTRL', 'SHIFT', 'ALT', 'NO CLIP APP', 'SAVE', 'CANCEL')


def key_button(label):
    return sym(f"echo_queue_key_{label.lower().replace(' ', '_')}_v1")
OURS = {PAGE, OVERLAY, ICON_TEXTURE}      # resources only EchoLazyTablet has

# Geometry in tablet canvas pixels (1024 px = 0.3 m).
PAGE_X, PAGE_Y, PAGE_W, PAGE_H = 12, 112, 1000, 528
DX = (PAGE_W - 942) // 2
BAR_Y = (662, 751)                        # the tab strip
TAB_SIZE = 89
FIRST_CENTER, LAST_CENTER = 172, 852      # outermost tab centres (5 tabs: 170 px apart)
MAX_TABS = 8                              # 8 tabs are 97 px apart; more would overlap
SCALE = .3 / 1024                         # touch areas are in metres

CARD = (2, 8, PAGE_W - 2, 520)
MODE_TILE = (44 + DX, 30, 164 + DX, 150)
BUTTON = (64 + DX, 198, 620 + DX, 284)          # FIND MATCH / CANCEL SEARCH
CLIP = (636 + DX, 198, 790 + DX, 284)            # CLIP: presses the player's clip keybind
CLIP_KEY = (798 + DX, 198, 878 + DX, 284)        # KEY: sets that keybind
CLOCK_FORMAT = (732, 48, 816, 100)               # 12H / 24H, in the title strip (overlay pixels)
KEYBOARD = (4, 106, 1020, 658)                   # the CLIP HOTKEY keyboard: the whole screen under the title strip
OVERLAY_H = 768                                  # (nothing past the tablet's edge is drawn: tried, 25 Oct)
COLUMNS = (118 + DX, 353 + DX, 589 + DX, 824 + DX)
ROW_CAPTION, ROW_BOX = (318, 350), (358, 468)

ATLAS = 0x79563b60c7895d4e                # the tablet UI atlas, 2048 x 3424 BC7
CARD_UV = (0.2520, 0.4632, 0.4927, 0.5686)
CARD_ACTIVE_UV = (0.0142, 0.4632, 0.2427, 0.5634)
TILE_UV = (0.831, 0.101, 0.961, 0.178)
SCREEN_UV = (0.015, 0.232, 0.515, 0.456)
ROW_ATLAS = 0x9385350a879a938b
ROW_BACK_UV, ROW_FILL_UV = (0.0215, 0.4609, 0.0801, 0.5215), (0.0078, 0.0059, 0.0117, 0.5039)
ROW_BORDER_UV = dict(tl=(0.793, 0.0059, 0.8535, 0.0664), t=(0.4688, 0.0059, 0.5293, 0.0664), tr=(0.5312, 0.0059, 0.5918, 0.0664),
                     l=(0.4062, 0.0684, 0.4668, 0.1289), r=(0.5312, 0.0684, 0.5918, 0.1289),
                     bl=(0.793, 0.1309, 0.8535, 0.1914), b=(0.4688, 0.1309, 0.5293, 0.1914), br=(0.5312, 0.1309, 0.5918, 0.1914))
FIND_FILL, FIND_BORDER = (.05, .46, .45, 1), (.2, .78, .75, 1)
CANCEL_FILL, CANCEL_BORDER = (.5, .24, .22, 1), (.66, .32, .29, 1)
CLIP_FILL, CLIP_BORDER = (.6, .32, .05, 1), (.9, .55, .15, 1)
TAB_IDLE_UV = (0.6904, 0.3645, 0.7393, 0.3937)
TAB_SELECTED_UV = (0.6899, 0.3297, 0.7393, 0.3595)
BGRA_TEMPLATE = 0x84b9a06506af66fe        # stock 128x128 B8G8R8A8 texture (header template)
DXGI_B8G8R8A8_UNORM, DXGI_B8G8R8A8_UNORM_SRGB = 87, 91

TEAL = (.05, .55, .54, 1)
TEXT, WHITE, BLACK = (.78, .78, .78, 1), (1, 1, 1, 1), (0, 0, 0, 1)
DIM, ACCENT = (.5, .54, .6, 1), (.2, .85, .82, 1)


class TabletError(Exception):
    """The tablet can't take the PLAY tab as it is; the message says why, in plain words."""


def colour(canvas, index, rgba):
    struct.pack_into('<4f', canvas.elements[index], 0x78, *rgba)


def fix_counts(canvas):
    """Header +0x28: elements using a texture; +0x2c/+0x30: vertex/index budget (one quad per
    panel or sprite and per character of text capacity)."""
    struct.pack_into('<I', canvas.header, 0x28, sum(Canvas.kind(e) in (3, 4, 8) for e in canvas.elements))
    quads = 0
    for e in canvas.elements:
        k = Canvas.kind(e)
        quads += 1 if k in (2, 3) else struct.unpack_from('<II', e, 0x90)[1] if k == 1 else 0
    struct.pack_into('<2I', canvas.header, 0x2c, 4 * quads, 6 * quads)


# ---- art ----
def atlas_image(m, name=ATLAS):
    import texture2ddecoder
    from PIL import Image
    cpu, gpu = m.get(TEX, name), m.get(TEX_GPU, name)
    if cpu is None or gpu is None:
        raise TabletError('The tablet art is missing from the game data.')
    w, h = struct.unpack_from('<II', cpu, 0xc4)
    if struct.unpack_from('<I', gpu, 128)[0] != 99:
        raise TabletError('The tablet art is in an unexpected format (different game version?).')
    return Image.frombytes('RGBA', (w, h), texture2ddecoder.decode_bc7(gpu[148:148 + (w // 4) * (h // 4) * 16], w, h), 'raw', 'BGRA')


def crop_uv(atlas, uv):
    u0, v0, u1, v1 = uv
    return atlas.crop((round(u0 * atlas.width), round(v0 * atlas.height), round(u1 * atlas.width), round(v1 * atlas.height)))


def tab_frame(atlas, uv):
    """A stock tab frame with its glyph painted out (bright pixels inside the border, and their
    edges, become that row's typical interior colour)."""
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
    """UVs of the bare tablet screen under `rect`: drawn there, it covers whatever is beneath."""
    u0, v0, u1, v1 = SCREEN_UV
    x0, y0, x1, y1 = rect
    return (u0 + x0 / 1024 * (u1 - u0), v0 + y0 / 768 * (v1 - v0), u0 + x1 / 1024 * (u1 - u0), v0 + y1 / 768 * (v1 - v0))


def icon_texture(atlas):
    """256 x 256: the PLAY tab (idle, selected) and the disc and house glyphs. -> (BGRA bytes, uvs)."""
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
    return Image.merge('RGBA', (b, g, r, a)).tobytes(), uvs


def texture_pair(m, w, h, pixels, fmt=DXGI_B8G8R8A8_UNORM_SRGB):
    """CGTextureResource (Win10 + Win10GPU): the stock BGRA texture's headers resized to w x h."""
    cpu = bytearray(m.get(TEX, BGRA_TEMPLATE) or b'')
    gpu = m.get(TEX_GPU, BGRA_TEMPLATE) or b''
    if len(cpu) != 256 or struct.unpack_from('<I', cpu, 0xd8)[0] != DXGI_B8G8R8A8_UNORM:
        raise TabletError('A stock texture EchoLazyTablet copies its format from is not as expected.')
    dds = bytearray(gpu[:148])
    if dds[:4] != b'DDS ' or dds[84:88] != b'DX10' or struct.unpack_from('<I', dds, 128)[0] != DXGI_B8G8R8A8_UNORM:
        raise TabletError('A stock texture EchoLazyTablet copies its format from is not as expected.')
    pitch = w * 4
    struct.pack_into('<III', dds, 12, h, w, pitch)
    struct.pack_into('<I', dds, 128, fmt)
    struct.pack_into('<II', cpu, 0xc4, w, h)
    struct.pack_into('<I', cpu, 0xd8, fmt)
    struct.pack_into('<II', cpu, 0xe8, w, h)
    struct.pack_into('<II', cpu, 0xf4, len(dds) + w * h * 4, pitch)
    return bytes(cpu), bytes(dds) + pixels


# ---- the tab bar ----
def find_tabs(nav, buttons):
    """Tabs in the bar, left to right: [dict(center, elements, rows, stock)]. A tab is the
    top-level elements sharing a tab-sized spot in the strip, and the tab-sized touch area of
    the tablet under it."""
    spots = []
    for i, row in enumerate(nav.elements):
        x0, y0, x1, y1 = Canvas.rect(row)
        if Canvas.parent(row) != -1 or not (40 <= x1 - x0 <= 200 and 40 <= y1 - y0 <= 150):
            continue
        if not BAR_Y[0] - 30 <= (y0 + y1) / 2 <= BAR_Y[1] + 30:
            continue
        cx = (x0 + x1) / 2
        spot = next((s for s in spots if abs(s['center'] - cx) <= 20), None)
        if spot is None:
            spot = dict(center=cx, elements=[], rows=[])
            spots.append(spot)
        spot['elements'].append(i)
    for r, row in enumerate(buttons.rows):
        if u64(row, 8) != ACTOR:
            continue
        x, y = struct.unpack_from('<2f', row, 0x80)
        hx, hy = struct.unpack_from('<2f', row, 0x9c)
        x, y, hx, hy = x / SCALE, -y / SCALE, hx / SCALE, hy / SCALE
        if not (BAR_Y[0] <= y <= BAR_Y[1] and hx >= 25 and hy >= 25):
            continue
        spot = min(spots, key=lambda s: abs(s['center'] - x), default=None)
        if spot and abs(spot['center'] - x) <= 30:
            spot['rows'].append(r)
    for s in spots:
        s['stock'] = all(i <= 8 for i in s['elements'])
    return sorted(spots, key=lambda s: s['center'])


def tab_centers(n):
    if n > MAX_TABS:
        raise TabletError(f'The tab bar would need {n} tabs; it only has room for {MAX_TABS}.')
    if n == 1:
        return [512.0]
    step = min(170.0, (LAST_CENTER - FIRST_CENTER) / (n - 1))
    return [512 + (i - (n - 1) / 2) * step for i in range(n)]


def pick_markers(canvases, targets):
    """For each target (name, element count after the build), the first element index whose name
    no other canvas with that many elements has at that index. -> {name: (count, index, marker)}."""
    out = {}
    for name, rows in targets.items():
        count = len(rows)
        rivals = [c for n, c in canvases.items() if n != name and len(c) == count]
        for i, row in enumerate(rows):
            marker = Canvas.name(row)
            if all(Canvas.name(r[i]) != marker for r in rivals):
                out[name] = (count, i, marker)
                break
        else:
            raise TabletError('Another tablet page looks exactly like one EchoLazyTablet needs to find; '
                              'the plugin could mix them up.')
    return out


# ---- the build ----
def build(m):
    """`m`: an echodata.Manifest without EchoLazyTablet. -> dict(resources, layout, notes, page)."""
    notes = []
    if m.get(CV, PAGE) is not None or m.get(CV, OVERLAY) is not None:
        raise TabletError('The PLAY tab is still in the game data; take EchoLazyTablet out first.')
    raw_root, raw_nav, raw_donor = m.get(CV, ROOT_CANVAS), m.get(CV, NAV_CANVAS), m.get(CV, TITLE_DONOR)
    raw_buttons = m.get(BUTTONS, LEVEL)
    if None in (raw_root, raw_nav, raw_donor, raw_buttons):
        raise TabletError('Parts of the hand tablet are missing from the game data.')
    try:
        root, nav, donor, buttons = Canvas(raw_root), Canvas(raw_nav), Canvas(raw_donor), Records(raw_buttons)
    except ValueError as e:
        raise TabletError(f'The hand tablet data could not be read ({e}).')
    if buttons.stride != 296:
        raise TabletError('The tablet touch-button table is not in the expected format.')
    stock_nav = len(nav.elements) >= 9 and all(Canvas.name(nav.elements[i]) == n for i, n in enumerate(
        (0xd66db0afd38fc682, 0xbe0c08782bee86be, 0x5453c273c2b6e9cd, 0x5b2b3c9f0a9985e0, 0x0c36d89ba7e45441,
         0x4900cc344f124e30, 0x11744364669c90ab, 0x1de409fe5b4ff4a0, 0xad01a8e7b7c0ae3d)))
    if not stock_nav:
        raise TabletError("The tab bar's own icons have been replaced by another mod; EchoLazyTablet can't place its tab safely.")
    if len(donor.elements) < 2:
        raise TabletError('A stock tablet canvas EchoLazyTablet copies its text style from has changed.')
    details = []     # technical, for the install log; `notes` are for players
    if len(root.elements) > 7:
        details.append(f'The tablet root has {len(root.elements) - 7} element(s) from other mods; kept, and the count stays the same.')
    if len(nav.elements) > 9:
        details.append(f'The tab bar has {len(nav.elements) - 9} element(s) from other mods; kept, and the count stays the same.')
    if len(buttons.rows) > STOCK_BUTTON_ROWS:
        details.append(f'The tablet has {len(buttons.rows) - STOCK_BUTTON_ROWS} touch area(s) from other mods; EchoLazyTablet adds its own after them.')

    old_root = [bytes(r) for r in root.elements]
    old_nav = [bytes(r) for r in nav.elements]
    old_rows = list(buttons.rows)
    text_template, panel_template = donor.elements[1], donor.elements[0]
    sprite_template = nav.elements[0]

    # Tabs: PLAY first, then every tab already there in its current order.
    tabs = find_tabs(nav, buttons)
    stock_found = [t for t in tabs if t['stock']]
    if len(stock_found) != 4 or any(len(t['rows']) != 1 for t in stock_found):
        raise TabletError("The stock tabs or their touch areas aren't where EchoLazyTablet expects them.")
    foreign = [t for t in tabs if not t['stock']]
    for t in foreign:
        details.append(f"Found another mod's tab at x={t['center']:.0f} ({len(t['elements'])} element(s), "
                       f"{len(t['rows'])} touch area(s)); it moves along with the stock tabs.")
        if not t['rows']:
            details.append('  That tab has no touch area EchoLazyTablet could match; only its icon moves.')
            notes.append("One of the other mods' tabs has no touch area EchoLazyTablet recognises; if it stops responding, "
                         'reinstall that mod and then Repair here.')
    if foreign:
        notes.insert(0, f"Another tablet mod is installed ({len(foreign)} tab{'s' if len(foreign) > 1 else ''}). "
                        'It keeps working: EchoLazyTablet only moves its tabs along to make room.')
    elif details:
        notes.insert(0, 'Another mod has changed your tablet. EchoLazyTablet builds around it and leaves it as it is.')
    centers = tab_centers(len(tabs) + 1)
    for t, new in zip(tabs, centers[1:]):
        dx = new - t['center']
        for i in t['elements']:
            x0, y0, x1, y1 = Canvas.rect(nav.elements[i])
            Canvas.set_rect(nav.elements[i], (x0 + dx, y0, x1 + dx, y1))
        for r in t['rows']:   # centred under the icon (stock ones start up to 1.4 px off)
            row = bytearray(buttons.rows[r])
            struct.pack_into('<f', row, 0x80, new * SCALE)
            buttons.rows[r] = bytes(row)
    atlas = atlas_image(m)
    icon_pixels, icon_uvs = icon_texture(atlas)
    c = centers[0]
    tab_rect = (c - TAB_SIZE / 2, BAR_Y[0], c + TAB_SIZE / 2, BAR_Y[1])

    # Everything EchoLazyTablet shows is one overlay canvas covering the tablet, shown by the root's
    # unused element 6, so no canvas another mod checks changes its element count. The PLAY
    # page is element 0 (its background) and its children; then the title strip and the tab.
    page = Canvas.empty(raw_donor, 1024, OVERLAY_H)
    struct.pack_into('<Q', page.header, 0, OVERLAY)
    group = -1

    def sprite(name, rect, uv, rgba, parent=None, hidden=False, texture=ATLAS):
        i = page.append(sprite_template, name, rect, parent=group if parent is None else parent, hidden=hidden)
        row = page.elements[i]
        row[0x24:0x34] = sprite_template[0x24:0x34]
        colour(page, i, rgba)
        struct.pack_into('<Q', row, 0x88, texture)
        struct.pack_into('<4f', row, 0x90, *uv)
        return i

    def text(name, words, rect, size, rgba, align=.5, hidden=False, capacity=0):
        i = page.label(text_template, name, words, rect, size, hidden=hidden, capacity=capacity)
        colour(page, i, rgba)
        struct.pack_into('<2f', page.elements[i], 0x9c, align, .5)
        struct.pack_into('<i', page.elements[i], 0x5c, group)
        return i

    group = sprite('echo_queue_background_v1', (PAGE_X, PAGE_Y, PAGE_X + PAGE_W, PAGE_Y + PAGE_H),
                   screen_uv((PAGE_X, PAGE_Y, PAGE_X + PAGE_W, PAGE_Y + PAGE_H)), WHITE, hidden=True)
    e = {}
    e['card'] = sprite('echo_queue_card_v1', CARD, CARD_UV, WHITE)
    e['card_active'] = sprite('echo_queue_card_active_v1', CARD, CARD_ACTIVE_UV, WHITE, hidden=True)
    tile = sprite('echo_queue_mode_tile_v1', MODE_TILE, TILE_UV, TEAL)
    tw = MODE_TILE[2] - MODE_TILE[0]
    sprite('echo_queue_mode_icon_v1', (16, 16, tw - 16, tw - 16), icon_uvs['disc'], WHITE, parent=tile, texture=ICON_TEXTURE)
    text('echo_queue_title_text_v1', 'ECHO ARENA', (190 + DX, 26, 620 + DX, 80), 52, WHITE, align=0)
    text('echo_queue_mode_text_v1', 'PUBLIC MATCH', (192 + DX, 80, 620 + DX, 110), 24, ACCENT, align=0)
    e['status'] = text('echo_queue_status_v1', 'TAP FIND MATCH TO QUEUE', (192 + DX, 112, 640 + DX, 146), 24, TEXT, align=0, capacity=96)
    e['timer'] = text('echo_queue_timer_v1', 'READY', (620 + DX, 30, 896 + DX, 112), 64, WHITE, align=1, capacity=16)
    e['timer_caption'] = text('echo_queue_timer_caption_v1', 'STATUS', (620 + DX, 112, 896 + DX, 142), 20, ACCENT, align=1, capacity=24)

    def lobby_row(name, rect, fill, border, title, hidden=False, size=46):
        x0, y0, x1, y1 = rect
        w, h = x1 - x0, y1 - y0
        row = sprite(name, rect, ROW_BACK_UV, WHITE, hidden=hidden, texture=ROW_ATLAS)
        sprite(name + '_fill', (0, 0, w, h), ROW_FILL_UV, fill, parent=row, texture=ROW_ATLAS)
        k = 32
        pieces = dict(tl=(0, 0, k, k), t=(k, 0, w - k, k), tr=(w - k, 0, w, k), l=(0, k, k, h - k), r=(w - k, k, w, h - k),
                      bl=(0, h - k, k, h), b=(k, h - k, w - k, h), br=(w - k, h - k, w, h))
        for piece, r in pieces.items():
            sprite(f'{name}_{piece}', r, ROW_BORDER_UV[piece], border, parent=row, texture=ROW_ATLAS)
        label = text(name + '_title', title, (8, 0, w - 8, h), size, WHITE)
        struct.pack_into('<i', page.elements[label], 0x5c, row)
        return row, label

    e['find'], e['find_text'] = lobby_row('echo_queue_find_row_v1', BUTTON, FIND_FILL, FIND_BORDER, 'FIND MATCH')
    e['cancel'], e['cancel_text'] = lobby_row('echo_queue_cancel_row_v1', BUTTON, CANCEL_FILL, CANCEL_BORDER, 'CANCEL SEARCH', hidden=True)
    e['clip'], e['clip_text'] = lobby_row('echo_queue_clip_row_v1', CLIP, CLIP_FILL, CLIP_BORDER, 'CLIP')
    lobby_row('echo_queue_clip_key_row_v1', CLIP_KEY, CLIP_FILL, CLIP_BORDER, 'KEY', size=30)

    def action_box(key, cx, caption, label, icon=None, busy=True, capacity=0):
        e[key + '_caption'] = text(f'echo_queue_caption_{cx}_v1', caption, (cx - 115, ROW_CAPTION[0], cx + 115, ROW_CAPTION[1]), 22, WHITE, capacity=capacity)
        rect = (cx - 96, ROW_BOX[0] + 8, cx + 96, ROW_BOX[1] + 8)
        w, h = rect[2] - rect[0], rect[3] - rect[1]
        sprite(f'echo_queue_{key}_base_v1', rect, CARD_UV, WHITE)
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
    stats_rect = action_box('stats', COLUMNS[2], 'HOURS TODAY', '0 MINS', busy=False, capacity=16)

    # GRAPHICS: HIGH and LOW side by side in the last column; the plugin lights the chosen one.
    cx = COLUMNS[3]
    e['gfx_caption'] = text(f'echo_queue_caption_{cx}_v1', 'GRAPHICS', (cx - 115, ROW_CAPTION[0], cx + 115, ROW_CAPTION[1]), 22, WHITE, capacity=24)
    gfx_rects = {}
    for key, label, x0, x1 in (('gfx_high', 'HIGH', cx - 96, cx - 4), ('gfx_low', 'LOW', cx + 4, cx + 96)):
        rect = (x0, ROW_BOX[0] + 8, x1, ROW_BOX[1] + 8)
        w, h = x1 - x0, rect[3] - rect[1]
        sprite(f'echo_queue_{key}_base_v1', rect, CARD_UV, WHITE)
        e[key] = sprite(f'echo_queue_{key}_box_v1', rect, CARD_ACTIVE_UV, WHITE, hidden=True)
        sprite(f'echo_queue_{key}_fill_v1', (6, 6, w - 6, h - 6), ROW_FILL_UV, (*FIND_FILL[:3], .55), parent=e[key], texture=ROW_ATLAS)
        e[key + '_text'] = text(f'echo_queue_{key}_text_v1', label, rect, 34, WHITE)
        gfx_rects[key] = rect

    # The CLIP HOTKEY page: shown instead of the PLAY page (first time, or from KEY), a keyboard over
    # the whole screen under the title strip to pick the clip recorder's hotkey with. The plugin
    # puts the combo picked so far in the title strip and lights the modifiers that are on.
    page_group = group
    group = e['keyboard'] = sprite('echo_queue_keyboard_v1', KEYBOARD, screen_uv(KEYBOARD), WHITE, parent=-1, hidden=True)
    kw, kh_all = KEYBOARD[2] - KEYBOARD[0], KEYBOARD[3] - KEYBOARD[1]
    sprite('echo_queue_keyboard_card_v1', (0, 0, kw, kh_all), CARD_UV, WHITE)
    key_rects = {}
    pitch, kh, row_pitch, top = 84, 80, 90, 6

    def key(label, rect, size, fill=None, toggle=False):
        x0, y0, x1, y1 = rect
        w, h = x1 - x0, y1 - y0
        n = label.lower().replace(' ', '_')
        if fill:
            sprite(f'echo_queue_key_{n}_base_v1', rect, ROW_FILL_UV, fill, texture=ROW_ATLAS)
        else:
            sprite(f'echo_queue_key_{n}_base_v1', rect, CARD_UV, WHITE)
        if toggle:   # lit while that modifier is on
            e[f'mod_{n}'] = lit = sprite(f'echo_queue_key_{n}_on_v1', rect, CARD_ACTIVE_UV, WHITE, hidden=True)
            sprite(f'echo_queue_key_{n}_on_fill_v1', (4, 4, w - 4, h - 4), ROW_FILL_UV, (*FIND_FILL[:3], .7), parent=lit, texture=ROW_ATLAS)
        text(f'echo_queue_key_{n}_text_v1', label, rect, size, WHITE)
        key_rects[label] = rect

    for r, labels in enumerate(KEYBOARD_ROWS):
        y0 = top + r * row_pitch
        x = kw / 2 - pitch * len(labels) / 2
        for i, label in enumerate(labels):
            key(label, (x + i * pitch + 4, y0, x + (i + 1) * pitch - 4, y0 + kh), 34 if len(label) > 1 else 46)
    y0 = top + len(KEYBOARD_ROWS) * row_pitch
    for label, x0, x1 in (('CTRL', 16, 156), ('SHIFT', 166, 306), ('ALT', 316, 456)):
        key(label, (x0, y0, x1, y0 + kh), 34, toggle=True)
    key('NO CLIP APP', (466, y0, 686, y0 + kh), 30, fill=CLIP_FILL)   # for players without clipping software
    key('SAVE', (kw - 320, y0, kw - 170, y0 + kh), 40, fill=FIND_FILL)
    key('CANCEL', (kw - 160, y0, kw - 16, y0 + kh), 32, fill=CANCEL_FILL)
    key_rects = {k: (r[0] + KEYBOARD[0], r[1] + KEYBOARD[1], r[2] + KEYBOARD[0], r[3] + KEYBOARD[1]) for k, r in key_rects.items()}

    # The title strip (a cover over the stock title, PLAY's own title) and the tab, top level.
    group = -1
    title_area = (54, 22, 822, 103)
    cover = sprite('echo_queue_title_cover_v1', title_area, screen_uv(title_area), WHITE, hidden=True)
    header = page.append(panel_template, 'echo_queue_header_background_v1', (60, 40, 210, 108), hidden=True)
    colour(page, header, WHITE)
    title = text('echo_queue_title_v1', 'PLAY', (76, 40, 210, 108), 44, BLACK, align=0, hidden=True, capacity=64)
    subtitle = text('echo_queue_subtitle_v1', 'ARENA MATCHMAKING', (236, 40, 800, 108), 30, DIM, align=0, hidden=True, capacity=64)
    # The clock at the strip's right end, and the 12H / 24H switch next to it (it names the format shown).
    clock = text('echo_queue_clock_v1', '00:00', (560, 40, 722, 108), 36, WHITE, align=1, hidden=True, capacity=16)
    clock_format = sprite('echo_queue_clock_format_v1', CLOCK_FORMAT, CARD_ACTIVE_UV, WHITE, hidden=True)
    clock_format_text = text('echo_queue_clock_format_text_v1', '24H', CLOCK_FORMAT, 26, WHITE, hidden=True, capacity=8)
    tab_icon = sprite('echo_queue_tab_icon_v1', tab_rect, icon_uvs['tab_idle'], WHITE, texture=ICON_TEXTURE)
    tab_selected = sprite('echo_queue_tab_selected_v1', tab_rect, icon_uvs['tab_selected'], WHITE, hidden=True, texture=ICON_TEXTURE)

    # The root's element 6 (stock: a hidden 96 px panel above the tablet, unused) becomes the
    # overlay's child-canvas element; it keeps its name and index (VR-tested 23 Oct).
    six = root.elements[ROOT_SLOT]
    if Canvas.name(six) != ROOT_SLOT_NAME or Canvas.kind(six) != 2 or Canvas.kind(old_root[1]) != 7:
        raise TabletError("The tablet root's spare element has been changed by another mod; EchoLazyTablet has nowhere to hang its tab.")
    slot = bytearray(old_root[1])
    slot[0:8] = six[0:8]
    struct.pack_into('<I', slot, 0x10, 0)
    struct.pack_into('<4f', slot, 0x24, 0, 0, 0, 0)
    struct.pack_into('<4f', slot, 0x34, 0, 0, 1024, OVERLAY_H)
    struct.pack_into('<iI', slot, 0x5c, -1, ROOT_SLOT)
    struct.pack_into('<Q', slot, 0x78, OVERLAY)
    root.elements[ROOT_SLOT] = slot

    for canvas in (root, nav, page):
        fix_counts(canvas)
        canvas.check()
    # No canvas but our own changes its element count; nothing changes but element 6 and tab positions.
    assert len(root.elements) == len(old_root) and len(nav.elements) == len(old_nav)
    assert all(bytes(r) == o for i, (r, o) in enumerate(zip(root.elements, old_root)) if i != ROOT_SLOT)
    moved = {i for t in tabs for i in t['elements']}
    for i, (old, new) in enumerate(zip(old_nav, nav.elements)):
        assert old == bytes(new) or (i in moved and old[:0x34] == new[:0x34] and old[0x44:] == new[0x44:])

    source = buttons.rows[stock_found[0]['rows'][0]]

    def add_button(name_sym, rect, on_page=True):
        row = bytearray(source)
        struct.pack_into('<Q', row, 0, name_sym)
        x0, y0, x1, y1 = rect
        if on_page:
            x0, y0, x1, y1 = x0 + PAGE_X, y0 + PAGE_Y, x1 + PAGE_X, y1 + PAGE_Y
        struct.pack_into('<2f', row, 0x80, (x0 + x1) / 2 * SCALE, -(y0 + y1) / 2 * SCALE)
        struct.pack_into('<2f', row, 0x9c, (x1 - x0) / 2 * SCALE, (y1 - y0) / 2 * SCALE)
        buttons.rows.append(bytes(row))

    add_button(TAB_BUTTON, tab_rect, on_page=False)
    add_button(START_BUTTON, BUTTON)
    add_button(CANCEL_BUTTON, BUTTON)
    add_button(LOBBY_BUTTON, lobby_rect)
    add_button(STATS_BUTTON, stats_rect)
    add_button(PRIVATE_BUTTON, private_rect)
    add_button(CLIP_BUTTON, CLIP)
    add_button(CLIP_KEY_BUTTON, CLIP_KEY)
    for label, rect in key_rects.items():   # the keyboard is in overlay pixels, not the page's
        add_button(key_button(label), rect, on_page=False)
    add_button(GFX_HIGH_BUTTON, gfx_rects['gfx_high'])
    add_button(GFX_LOW_BUTTON, gfx_rects['gfx_low'])
    add_button(CLOCK_BUTTON, CLOCK_FORMAT, on_page=False)
    moved_rows = {r for t in tabs for r in t['rows']}
    for r, old in enumerate(old_rows):
        assert r in moved_rows or buttons.rows[r] == old

    icon_cpu, icon_gpu = texture_pair(m, 256, 256, icon_pixels)
    resources = [(CV, ROOT_CANVAS, root.serialize()), (CV, NAV_CANVAS, nav.serialize()), (CV, OVERLAY, page.serialize()),
                 (BUTTONS, LEVEL, buttons.serialize()), (TEX, ICON_TEXTURE, icon_cpu), (TEX_GPU, ICON_TEXTURE, icon_gpu)]
    e.update(page=page_group, cover=cover, header=header, title=title, subtitle=subtitle, clock=clock, clock_format=clock_format,
             clock_format_text=clock_format_text, tab_icon=tab_icon, tab_selected=tab_selected)

    # How the plugin will recognise each canvas, given every canvas in the game after this build.
    final = {}
    for entry in m.entries:
        if entry[0] == CV:
            try:
                final[entry[1]] = Canvas(m.get(CV, entry[1])).elements
            except ValueError:
                pass
    final[ROOT_CANVAS], final[NAV_CANVAS], final[OVERLAY] = root.elements, nav.elements, page.elements
    missing = [n for n in STOCK_PAGES if n not in final]
    if missing:
        raise TabletError('Some stock tablet pages are missing from the game data.')
    kinds = (ROOT_CANVAS, NAV_CANVAS, OVERLAY) + STOCK_PAGES
    markers = pick_markers(final, {n: final[n] for n in kinds})
    hide_selected = [p[1] for p in STOCK_TAB_PAIRS]
    others_selected = [i for t in foreign for i in t['elements'] if Canvas.hidden(nav.elements[i])]
    other_tabs = sorted({u64(buttons.rows[r], 0) for t in foreign for r in t['rows']})
    layout = dict(version=2, canvases=[markers[n] for n in kinds], root_slot=ROOT_SLOT,
                  nav_stock_selected=hide_selected, nav_other_selected=others_selected, other_tabs=other_tabs,
                  overlay_elements=len(page.elements))
    return dict(resources=resources, layout=layout, notes=notes, details=details, page=e, tabs=len(tabs) + 1)


def texture_image(cpu, gpu):
    """The top mip of a CGTextureResource pair as a PIL RGBA image (BC1/BC3/BC7 or 8-bit BGRA), or None."""
    import texture2ddecoder
    from PIL import Image
    if not cpu or not gpu or len(gpu) < 148 or gpu[:4] != b'DDS ':
        return None
    w, h = struct.unpack_from('<II', cpu, 0xc4)
    fmt = struct.unpack_from('<I', gpu, 128)[0]
    body = gpu[148:]
    blocks = ((w + 3) // 4) * ((h + 3) // 4)
    decoders = {71: (texture2ddecoder.decode_bc1, 8), 72: (texture2ddecoder.decode_bc1, 8),
                77: (texture2ddecoder.decode_bc3, 16), 78: (texture2ddecoder.decode_bc3, 16),
                98: (texture2ddecoder.decode_bc7, 16), 99: (texture2ddecoder.decode_bc7, 16)}
    if fmt in decoders:
        decode, size = decoders[fmt]
        return Image.frombytes('RGBA', (w, h), decode(body[:blocks * size], w, h), 'raw', 'BGRA')
    if fmt in (87, 91):
        return Image.frombytes('RGBA', (w, h), body[:w * h * 4], 'raw', 'BGRA')
    return None


def tab_preview(m, built):
    """The tab bar as it will be, left to right, for the installer's window:
    [dict(png=data URI or None, text=label or '', ours, stock)]."""
    import base64
    import io
    res = {(t, n): d for t, n, d in built['resources']}
    nav = Canvas(res[(CV, NAV_CANVAS)])
    overlay = Canvas(res[(CV, OVERLAY)])
    buttons = Records(res[(BUTTONS, LEVEL)])
    cache = {}

    def image(texture):
        if texture not in cache:
            cpu = res.get((TEX, texture)) or m.get(TEX, texture)
            gpu = res.get((TEX_GPU, texture)) or m.get(TEX_GPU, texture)
            cache[texture] = texture_image(cpu, gpu)
        return cache[texture]

    def describe(canvas, rows, ours=False, stock=False):
        out = dict(png=None, text='', ours=ours, stock=stock)
        for row in rows:
            if Canvas.hidden(row):
                continue
            if Canvas.kind(row) == 3:
                img = image(u64(row, 0x88))
                if img is not None:
                    u0, v0, u1, v1 = struct.unpack_from('<4f', row, 0x90)
                    crop = img.crop((round(u0 * img.width), round(v0 * img.height), round(u1 * img.width), round(v1 * img.height)))
                    crop.thumbnail((96, 96))
                    buf = io.BytesIO()
                    crop.save(buf, 'PNG')
                    out['png'] = 'data:image/png;base64,' + base64.b64encode(buf.getvalue()).decode()
                    return out
            elif Canvas.kind(row) == 1:
                slot = struct.unpack_from('<I', row, 0x90)[0]
                start = struct.unpack_from('<I', canvas.text_offsets, slot * 4)[0]
                out['text'] = bytes(canvas.text[start:start + 32]).split(b'\0')[0].decode('utf-8', 'replace')
        return out

    tabs = [(t['center'], describe(nav, [nav.elements[i] for i in t['elements']], stock=t['stock'])) for t in find_tabs(nav, buttons)]
    x0, _, x1, _ = Canvas.rect(overlay.elements[built['page']['tab_icon']])
    tabs.append(((x0 + x1) / 2, describe(overlay, [overlay.elements[built['page']['tab_icon']]], ours=True)))
    return [d for _, d in sorted(tabs, key=lambda t: t[0])]


def layout_ini(layout):
    """The layout as the plugin reads it (plugins\\EchoLazyTablet.layout)."""
    lines = ['; Written by the EchoLazyTablet installer for this PC\'s tablet. Repair rewrites it.', '[tablet]',
             f"version={layout['version']}"]
    names = ('root', 'nav', 'overlay', 'lobby', 'rows', 'settings', 'title')
    for name, (count, index, marker) in zip(names, layout['canvases']):
        lines.append(f'{name}={count},{index},{marker:016x}')
    lines.append(f"root_slot={layout['root_slot']}")
    lines.append(f"overlay_elements={layout['overlay_elements']}")
    lines.append('nav_stock_selected=' + ','.join(map(str, layout['nav_stock_selected'])))
    lines.append('nav_other_selected=' + ','.join(map(str, layout['nav_other_selected'])))
    lines.append('other_tabs=' + ','.join(f'{n:016x}' for n in layout.get('other_tabs', ())))  # their tab buttons
    return '\r\n'.join(lines) + '\r\n'


def page_header(built):
    """src/generated/queue_tab.h: the button names and the overlay's element indices."""
    e = built['page']
    return ('// Generated by installer/tablet.py -- do not edit.\n#pragma once\n#include <cstdint>\n'
            f'constexpr uint64_t QUEUE_TAB=0x{TAB_BUTTON:016x}, QUEUE_START=0x{START_BUTTON:016x}, '
            f'QUEUE_CANCEL=0x{CANCEL_BUTTON:016x}, QUEUE_LOBBY=0x{LOBBY_BUTTON:016x}, QUEUE_PRIVATE=0x{PRIVATE_BUTTON:016x}, '
            f'QUEUE_STATS=0x{STATS_BUTTON:016x}, QUEUE_GFX_HIGH=0x{GFX_HIGH_BUTTON:016x}, QUEUE_GFX_LOW=0x{GFX_LOW_BUTTON:016x}, QUEUE_CLIP=0x{CLIP_BUTTON:016x}, QUEUE_CLIP_KEY=0x{CLIP_KEY_BUTTON:016x}, QUEUE_CLOCK=0x{CLOCK_BUTTON:016x};\n'
            f"constexpr unsigned QUEUE_OVERLAY_ELEMENTS={built['layout']['overlay_elements']}, QUEUE_LAYOUT_VERSION={built['layout']['version']};\n"
            + ''.join(f'constexpr unsigned QUEUE_PAGE_{k.upper()}={v};\n' for k, v in e.items())
            + 'struct QueueKey { uint64_t name; const char* label; };\nconstexpr QueueKey QUEUE_KEYS[] = {'
            + ', '.join(f'{{0x{key_button(k):016x}, "{k}"}}' for k in [k for row in KEYBOARD_ROWS for k in row] + list(KEYBOARD_EXTRA))
            + '};\n')


if __name__ == '__main__':
    import argparse
    from pathlib import Path
    import patcher
    a = argparse.ArgumentParser(description='Regenerate the plugin\'s page constants from a game install.')
    a.add_argument('--game', required=True)
    a.add_argument('--header', required=True)
    a = a.parse_args()
    game = patcher.Game(Path(a.game))
    built = build(patcher.without_echoqueue(game)[0])
    Path(a.header).write_text(page_header(built))
    print('wrote', a.header)
