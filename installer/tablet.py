"""Builds EchoLazyTablet's PLAY tab on top of the hand tablet as it is now, other tablet mods included.

build(manifest) takes a manifest with no EchoLazyTablet in it (the patcher takes ours out first) and
returns the resources to add, the layout file the plugin reads, and notes on what it found:

  * the root canvas, tab bar and touch-button table are whatever the manifest holds now (stock
    or another mod's). No canvas changes its element count (other mods' plugins find the tablet
    by exact counts, e.g. Echo Arcade's root 10 / tab bar 13): everything EchoLazyTablet shows is one
    overlay canvas of its own, shown by the root's unused stock element 6 (a hidden panel,
    turned into a child-canvas element). Touch areas are appended to the button table;
  * every tab in the tab bar (stock or another mod's: anything tab-sized in the bar strip, with
    the tab-sized touch area under it) is spread evenly with SFX and PLAY first, the touch areas moved
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
START_BUTTON = sym('echo_queue_start_button_v1')    # ARENA
COMBAT_BUTTON = sym('echo_queue_combat_button_v1')
CANCEL_BUTTON = sym('echo_queue_cancel_button_v1')
LOBBY_BUTTON = sym('echo_queue_lobby_button_v1')
STATS_BUTTON = sym('echo_queue_stats_button_v1')
PRIVATE_BUTTON = sym('echo_queue_private_button_v1')
GFX_HIGH_BUTTON = sym('echo_queue_gfx_high_button_v1')
GFX_LOW_BUTTON = sym('echo_queue_gfx_low_button_v1')
CLIP_BUTTON = sym('echo_queue_clip_button_v1')
CLIP_KEY_BUTTON = sym('echo_queue_clip_key_button_v1')    # opens the CLIP HOTKEY page
CLOCK_BUTTON = sym('echo_queue_clock_format_button_v1')  # the title strip's 12H / 24H switch (SFX page)
PLAY_CLOCK_BUTTON = sym('echo_queue_play_clock_button_v1')  # the PLAY page's clock: tap for 12 / 24 hour
CLOCK_12_BUTTON = sym('echo_queue_clock_12_button_v1')      # 12H and 24H, beside it
CLOCK_24_BUTTON = sym('echo_queue_clock_24_button_v1')
UPDATE_BUTTON = sym('echo_queue_update_button_v1')        # UPDATE AVAILABLE: opens the latest release in the browser
SOUND_TAB_BUTTON = sym('echo_queue_sound_tab_button_v1')  # the SFX tab, next to PLAY
SOUND_EDIT_BUTTON = sym('echo_queue_sound_edit_button_v1')
SOUND_PADS = 8                            # the SFX page's buttons, each with its own keybind


def sound_button(i):
    return sym(f'echo_queue_sound_{i}_button_v1')


# The HOLO tab (only with the hologram chassis mod installed): the hologram arms' colour for blue
# team, orange team and the lobby, picked like in a paint app (12 Nov layout). Left: which one is
# being edited, a round preview with its hex code, HOLOGRAM BLUE (back to the hologram's own).
# Middle: a smooth colour field (hue across, light to dark down) and a white-to-black bar, both
# tapped through an invisible grid of touch areas (a ring marks the pick); under them word buttons
# to fine-tune it (< COLOUR / COLOUR >, DARKER / LIGHTER, PALER / BOLDER: one step a tap, hold to
# keep going), each filled with the colour it leads to (the plugin tints them). Right: RAINBOW (the whole arms cycle
# round the colour wheel) and its speed, and ARMS SOLID / SEE-THROUGH / HANDS ONLY (next start).
HOLO_TAB_BUTTON = sym('echo_queue_holo_tab_button_v1')
HOLO_BARS = ('blue', 'orange', 'lobby')
HOLO_BAR_LABELS = ('BLUE TEAM', 'ORANGE TEAM', 'LOBBY')
HOLO_SPEEDS = 5
HOLO_LOOKS = ('solid', 'clear', 'hands', 'clear_hands')   # ARMS: SOLID / SEE-THROUGH / SOLID HANDS / CLEAR HANDS
HOLO_FINE = ('hue', 'light', 'sat')                 # the fine-tune pairs: COLOUR, BRIGHTNESS, VIVIDNESS
HOLO_FIELD_COLS, HOLO_FIELD_ROWS, HOLO_GREYS = 20, 9, 12
HOLO_FIELD_TOP, HOLO_FIELD_SPAN = .92, .84          # the field's lightness: top edge, top to bottom
# Every control, in the order the plugin's arrays use (each has a touch area and a highlight).
HOLO_CONTROLS = ([f'edit_{b}' for b in HOLO_BARS] + ['reset', 'rainbow'] + [f'speed_{i}' for i in range(HOLO_SPEEDS)]
                 + [f'look_{k}' for k in HOLO_LOOKS] + [f'{k}_{x}' for k in HOLO_FINE for x in ('minus', 'plus')]
                 + [f'field_{r}_{c}' for r in range(HOLO_FIELD_ROWS) for c in range(HOLO_FIELD_COLS)]
                 + [f'grey_{i}' for i in range(HOLO_GREYS)])


def holo_field_rgb(u, v):
    """The colour field at (u, v) in 0-1 (also what the plugin picks for a tap: the touch area's centre)."""
    import colorsys
    return colorsys.hls_to_rgb(u % 1, HOLO_FIELD_TOP - HOLO_FIELD_SPAN * v, 1)


def holo_grey(u):
    """The white-to-black bar at u in 0-1: each touch area's centre is exactly one of its HOLO_GREYS greys."""
    n = HOLO_GREYS
    return 1 - min(1, max(0, (u * n - .5) / (n - 1)))


# The hologram chassis mod's wire material (echo-modding-tool holo_whole.rs): its being in the
# game is what puts the HOLO tab in (ECHOQUEUE_HOLO=1 forces it, =0 leaves it out).
HOLO_MATERIAL = (0xe3e0266f1911dafa, 0x5182867f02da1080)


def holo_button(key):
    return sym(f'echo_queue_holo_{key}_button_v1')


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
# The PLAY page, on one grid: everything between x 28 and 972, rows 16 px apart.
MODE_TILE = (28, 22, 148, 142)
TIME = (500, 22, 700, 142)                      # the clock's card, beside the mode; right of it is free (tap: 12 / 24 hour)
BUTTON = (28, 166, 632, 250)                    # CANCEL SEARCH (where ARENA and COMBAT are)
COMBAT = (28, 166, 322, 250)                    # COMBAT: queues for Echo Combat
ARENA = (338, 166, 632, 250)                    # ARENA: queues for Echo Arena (in the middle)
CLIP = (648, 166, 852, 250)                     # CLIP: presses the player's clip keybind
CLIP_KEY = (868, 166, 972, 250)                 # KEY: sets that keybind
TILE_W, TILE_GAP, TILE_Y = 176, 16, (270, 506)  # the five boxes along the bottom
UPDATE = (808, 60, 984, 104)                    # UPDATE AVAILABLE (a pill beside 12H / 24H): only while a newer EchoLazyTablet is on GitHub
CLOCK_FORMAT = (732, 48, 816, 100)               # 12H / 24H, in the title strip (overlay pixels)
KEYBOARD = (4, 106, 1020, 658)                   # the CLIP HOTKEY keyboard: the whole screen under the title strip
SOUND_EDIT = (760, 20, 960, 98)                  # the SFX page's EDIT / DONE
SOUND_PAD_W, SOUND_PAD_H, SOUND_PAD_GAP = 224, 186, 14
SOUND_PAD_ORIGIN = (30, 110)                     # the SFX page's top left button
HOLO_FIELD = (252, 22, 772, 330)                 # the colour field (page pixels)
HOLO_GREY_BAR = (252, 340, 772, 370)             # the white-to-black bar under it
HOLO_BTN, HOLO_LIT, HOLO_EDGE = (.16, .18, .22, 1), (.05, .46, .45, 1), (.2, .82, .78, 1)
OVERLAY_H = 768                                  # (nothing past the tablet's edge is drawn: tried, 25 Oct)

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
UPDATE_DOT = (.25, .95, .35, 1)   # UPDATE AVAILABLE's dot (green)
UPDATE_DOT_NONE = (.95, .2, .2, 1)  # NO UPDATE's dot (red)
UPDATE_BOX = (.14, .15, .17, 1)   # ...and its box: a dark grey, just lighter than the page
UPDATE_EDGE = (.06, .065, .08, 1)  # ...with a darker outline
TAB_IDLE_UV = (0.6904, 0.3645, 0.7393, 0.3937)
TAB_SELECTED_UV = (0.6899, 0.3297, 0.7393, 0.3595)
BGRA_TEMPLATE = 0x84b9a06506af66fe        # stock 128x128 B8G8R8A8 texture (header template)
DXGI_B8G8R8A8_UNORM, DXGI_B8G8R8A8_UNORM_SRGB = 87, 91
ICON_W, ICON_H = 512, 1024

TEAL = (.05, .55, .54, 1)
TEXT, WHITE, BLACK = (.78, .78, .78, 1), (1, 1, 1, 1), (0, 0, 0, 1)
GREY = (.07, .08, .1, 1)
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


def holo_texture(tex, uvs):
    from PIL import Image, ImageDraw
    import colorsys

    def place(key, im, x, y):
        tex.alpha_composite(im, (x, y))
        uvs[key] = (x / ICON_W, y / ICON_H, (x + im.width) / ICON_W, (y + im.height) / ICON_H)

    def rounded(im, radius):
        big = Image.new('L', (im.width * 4, im.height * 4), 0)
        ImageDraw.Draw(big).rounded_rectangle((0, 0, big.width - 1, big.height - 1), radius=radius * 4, fill=255)
        im.putalpha(big.resize(im.size, Image.LANCZOS))
        return im

    def picture(w, h, fn):
        im = Image.new('RGB', (w, h))
        im.putdata([tuple(round(255 * c) for c in fn(x / (w - 1), y / max(1, h - 1))) for y in range(h) for x in range(w)])
        return im.convert('RGBA')

    def shape(w, h, radius):
        return rounded(Image.new('RGBA', (w, h), (255, 255, 255, 255)), radius)

    place('holo_field', rounded(picture(512, 272, holo_field_rgb), 14), 0, 512)
    place('holo_grey', rounded(picture(512, 30, lambda u, v: (holo_grey(u),) * 3), 15), 0, 792)
    place('holo_rainbow', rounded(picture(256, 12, lambda u, v: colorsys.hsv_to_rgb(u, 1, 1)), 6), 0, 830)
    place('holo_pill', shape(208, 46, 23), 0, 850)
    place('holo_pill_short', shape(184, 46, 23), 216, 850)
    place('holo_box', shape(184, 96, 20), 0, 904)
    ring = Image.new('RGBA', (256, 256), (0, 0, 0, 0))   # white ring, thin dark edge each side
    d = ImageDraw.Draw(ring)
    d.ellipse((8, 8, 248, 248), fill=(20, 20, 20, 230))
    d.ellipse((16, 16, 240, 240), fill=(255, 255, 255, 255))
    d.ellipse((64, 64, 192, 192), fill=(20, 20, 20, 230))
    d.ellipse((72, 72, 184, 184), fill=(0, 0, 0, 0))
    place('holo_ring', ring.resize((64, 64), Image.LANCZOS), 192, 904)
    place('holo_tile', shape(82, 48, 14), 264, 904)


def icon_texture(atlas):
    """ICON_W x ICON_H: the PLAY and SFX tabs (idle, selected) and the disc and house glyphs. -> (BGRA bytes, uvs)."""
    from PIL import Image
    from disc_icon import chart, disc, house, lock, monitor, palette, potato, speaker, speed
    tex = Image.new('RGBA', (ICON_W, ICON_H), (0, 0, 0, 0))
    uvs = {}
    for col, (key, uv, glyph) in enumerate((('tab_idle', TAB_IDLE_UV, disc), ('tab_selected', TAB_SELECTED_UV, disc),
                                            ('sound_idle', TAB_IDLE_UV, speaker), ('sound_selected', TAB_SELECTED_UV, speaker))):
        frame = tab_frame(atlas, uv)
        w, h = frame.size
        g = glyph(round(h * .66))
        frame.alpha_composite(g, ((w - g.width) // 2, (h - g.height) // 2))
        tex.paste(frame, (col * 128, 0))
        uvs[key] = (col * 128 / ICON_W, 0, (col * 128 + w) / ICON_W, h / ICON_H)
    tex.alpha_composite(disc(120), (4, 132))
    uvs['disc'] = (4 / ICON_W, 132 / ICON_H, 124 / ICON_W, 252 / ICON_H)
    tex.alpha_composite(house(120), (132, 132))
    uvs['house'] = (132 / ICON_W, 132 / ICON_H, 252 / ICON_W, 252 / ICON_H)
    for col, (key, uv) in enumerate((('holo_idle', TAB_IDLE_UV), ('holo_selected', TAB_SELECTED_UV))):   # the HOLO tab
        frame = tab_frame(atlas, uv)
        w, h = frame.size
        g = palette(round(h * .66))
        frame.alpha_composite(g, ((w - g.width) // 2, (h - g.height) // 2))
        x = 260 + col * 124
        tex.paste(frame, (x, 132))
        uvs[key] = (x / ICON_W, 132 / ICON_H, (x + w) / ICON_W, (132 + h) / ICON_H)
    for col, (key, glyph) in enumerate((('lock', lock), ('chart', chart), ('speed', speed), ('monitor', monitor))):
        tex.alpha_composite(glyph(120), (col * 128 + 4, 260))
        uvs[key] = ((col * 128 + 4) / ICON_W, 260 / ICON_H, (col * 128 + 124) / ICON_W, 380 / ICON_H)
    tex.alpha_composite(potato(120), (4, 388))
    dot = Image.new('RGBA', (480, 480), (0, 0, 0, 0))   # a round dot (drawn big, then shrunk smooth), tinted per use
    from PIL import ImageDraw
    ImageDraw.Draw(dot).ellipse((8, 8, 472, 472), fill=(255, 255, 255, 255))
    tex.alpha_composite(dot.resize((120, 120), Image.LANCZOS), (132, 388))
    uvs['dot'] = (132 / ICON_W, 388 / ICON_H, 252 / ICON_W, 508 / ICON_H)
    # UPDATE AVAILABLE's box: a rounded rectangle at its own shape (so the corners aren't stretched), 1.25x size.
    bw, bh = round((UPDATE[2] - UPDATE[0]) * 1.25), round((UPDATE[3] - UPDATE[1]) * 1.25)
    pill = Image.new('RGBA', (bw * 4, bh * 4), (0, 0, 0, 0))
    ImageDraw.Draw(pill).rounded_rectangle((0, 0, bw * 4 - 1, bh * 4 - 1), radius=bh * 4 // 3, fill=(255, 255, 255, 255))
    tex.alpha_composite(pill.resize((bw, bh), Image.LANCZOS), (260, 388))
    uvs['pill'] = (260 / ICON_W, 388 / ICON_H, (260 + bw) / ICON_W, (388 + bh) / ICON_H)
    uvs['potato'] = (4 / ICON_W, 388 / ICON_H, 124 / ICON_W, 508 / ICON_H)
    # The HOLO page (12 Nov): its colour field, white-to-black bar and rainbow strip (each with
    # rounded ends), and white shapes the page tints: pills, the RAINBOW box and the pick's ring.
    holo_texture(tex, uvs)
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
def build(m, holo=None):
    """`m`: an echodata.Manifest without EchoLazyTablet. -> dict(resources, layout, notes, page).
    `holo`: put the HOLO tab in (default: when the hologram chassis mod is in `m`, or ECHOQUEUE_HOLO
    says so). Its page is built either way (hidden), so the overlay is the same for the plugin."""
    if holo is None:
        import os
        forced = os.environ.get('ECHOQUEUE_HOLO')
        holo = forced == '1' if forced in ('0', '1') else m.get(*HOLO_MATERIAL) is not None
    holo = bool(holo)
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

    # Tabs: SFX and PLAY first, then every tab already there in its current order.
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
    ours = 3 if holo else 2                 # SFX, PLAY (and HOLO) come first
    centers = tab_centers(len(tabs) + ours)
    for t, new in zip(tabs, centers[ours:]):
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
    c = centers[1]
    tab_rect = (c - TAB_SIZE / 2, BAR_Y[0], c + TAB_SIZE / 2, BAR_Y[1])
    c = centers[0]
    sound_tab_rect = (c - TAB_SIZE / 2, BAR_Y[0], c + TAB_SIZE / 2, BAR_Y[1])
    c = centers[2] if holo else centers[1]   # (without the mod: hidden, on PLAY's spot)
    holo_tab_rect = (c - TAB_SIZE / 2, BAR_Y[0], c + TAB_SIZE / 2, BAR_Y[1])

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
    # Top: the mode (disc tile, ECHO ARENA, what's going on), then the time and the status on the right.
    tile = sprite('echo_queue_mode_tile_v1', MODE_TILE, TILE_UV, TEAL)
    tw = MODE_TILE[2] - MODE_TILE[0]
    sprite('echo_queue_mode_icon_v1', (16, 16, tw - 16, tw - 16), icon_uvs['disc'], WHITE, parent=tile, texture=ICON_TEXTURE)
    text('echo_queue_title_text_v1', 'ECHO ARENA', (168, 24, 488, 76), 50, WHITE, align=0)
    text('echo_queue_mode_text_v1', 'PUBLIC MATCH', (170, 76, 580, 104), 24, ACCENT, align=0)
    e['status'] = text('echo_queue_status_v1', 'PICK ARENA OR COMBAT', (170, 106, 488, 140), 22, TEXT, align=0, capacity=96)
    sprite('echo_queue_play_clock_base_v1', TIME, CARD_UV, WHITE)
    clock_box = sprite('echo_queue_play_clock_box_v1', TIME, CARD_ACTIVE_UV, WHITE)
    sprite('echo_queue_play_clock_fill_v1', (6, 6, TIME[2] - TIME[0] - 6, TIME[3] - TIME[1] - 6), ROW_FILL_UV, (*FIND_FILL[:3], .55),
           parent=clock_box, texture=ROW_ATLAS)
    # 12-hour: the time with AM / PM centred under it; 24-hour: the time alone, centred in the card.
    e['play_clock'] = text('echo_queue_play_clock_v1', '00:00', (TIME[0], TIME[1] + 8, TIME[2], TIME[1] + 78), 56, WHITE, capacity=16)
    e['play_clock_ampm'] = text('echo_queue_play_clock_ampm_v1', 'AM', (TIME[0], TIME[1] + 74, TIME[2], TIME[3] - 8), 30, WHITE, capacity=8)
    e['play_clock_24'] = text('echo_queue_play_clock_24_v1', '00:00', TIME, 56, WHITE, hidden=True, capacity=16)
    # 12H on top and 24H under it, beside the card; the plugin lights the one in use.
    clock_rects = {}
    for key, label, y0, y1 in (('clock_12', '12H', TIME[1], TIME[1] + 56), ('clock_24', '24H', TIME[3] - 56, TIME[3])):
        rect = (TIME[2] + 12, y0, TIME[2] + 96, y1)
        w, h = rect[2] - rect[0], rect[3] - rect[1]
        sprite(f'echo_queue_{key}_base_v1', rect, CARD_UV, WHITE)
        e[key] = sprite(f'echo_queue_{key}_box_v1', rect, CARD_ACTIVE_UV, WHITE, hidden=True)
        sprite(f'echo_queue_{key}_fill_v1', (4, 4, w - 4, h - 4), ROW_FILL_UV, FIND_BORDER, parent=e[key], texture=ROW_ATLAS)
        text(f'echo_queue_{key}_text_v1', label, rect, 26, WHITE)
        clock_rects[key] = rect
    # UPDATE AVAILABLE, right of 12H / 24H: one line and a dot on a light grey rounded box
    # (always there; green and tappable when a newer release is out).
    ux0, uy0, ux1, uy1 = UPDATE
    edge = 5   # the outline: the same rounded shape 5 px bigger all round, the box on top of it
    e['update'] = sprite('echo_queue_update_edge_v1', (ux0 - edge, uy0 - edge, ux1 + edge, uy1 + edge), icon_uvs['pill'], UPDATE_EDGE, texture=ICON_TEXTURE)
    sprite('echo_queue_update_box_v1', (edge, edge, ux1 - ux0 + edge, uy1 - uy0 + edge), icon_uvs['pill'], UPDATE_BOX, parent=e['update'], texture=ICON_TEXTURE)
    # NO UPDATE with a red dot; DOWNLOAD V3.22 (the new version) with a green one (the plugin swaps them; only green can be tapped).
    e['update_text'] = text('echo_queue_update_text_v1', 'NO UPDATE', (ux0 + 10, uy0, ux1 - 26, uy1), 19, WHITE, capacity=24)
    dot = (ux1 - 24, uy0 + 15, ux1 - 10, uy0 + 29)
    e['update_dot_red'] = sprite('echo_queue_update_dot_red_v1', dot, icon_uvs['dot'], UPDATE_DOT_NONE, texture=ICON_TEXTURE)
    e['update_dot'] = sprite('echo_queue_update_dot_v1', dot, icon_uvs['dot'], UPDATE_DOT, hidden=True, texture=ICON_TEXTURE)

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

    e['find'], e['find_text'] = lobby_row('echo_queue_find_row_v1', ARENA, FIND_FILL, FIND_BORDER, 'ARENA')
    e['combat'], e['combat_text'] = lobby_row('echo_queue_combat_row_v1', COMBAT, FIND_FILL, FIND_BORDER, 'COMBAT')
    e['cancel'], e['cancel_text'] = lobby_row('echo_queue_cancel_row_v1', BUTTON, CANCEL_FILL, CANCEL_BORDER, 'CANCEL SEARCH', hidden=True)
    e['clip'], e['clip_text'] = lobby_row('echo_queue_clip_row_v1', CLIP, CLIP_FILL, CLIP_BORDER, 'CLIP')
    lobby_row('echo_queue_clip_key_row_v1', CLIP_KEY, CLIP_FILL, CLIP_BORDER, 'KEY', size=30)

    # Bottom: five boxes the same size, each with its name at the top, an icon, and its value or buttons.
    def tile_rect(n):
        x0 = 28 + n * (TILE_W + TILE_GAP)
        return x0, TILE_Y[0], x0 + TILE_W, TILE_Y[1]

    def tile(key, n, caption, label, icon, busy=True, lit=True, capacity=0, card_h=None):
        """card_h: the card stops that far down (what's under it, e.g. GRAPHICS' buttons, stands on its own)."""
        rect = tile_rect(n)
        x0, y0, x1, y1 = rect
        w, h = x1 - x0, card_h or y1 - y0
        card = (x0, y0, x1, y0 + h)
        sprite(f'echo_queue_{key}_base_v1', card, CARD_UV, WHITE)
        if lit:   # (hidden by the plugin while it can't be used)
            e[key] = sprite(f'echo_queue_{key}_box_v1', card, CARD_ACTIVE_UV, WHITE)
            sprite(f'echo_queue_{key}_fill_v1', (6, 6, w - 6, h - 6), ROW_FILL_UV, (*FIND_FILL[:3], .55), parent=e[key], texture=ROW_ATLAS)
        side = 72
        icon_rect = (x0 + (w - side) / 2, y0 + 54, x0 + (w + side) / 2, y0 + 54 + side)
        e[key + '_icon'] = sprite(f'echo_queue_{key}_icon_v1', icon_rect, icon_uvs[icon], WHITE, texture=ICON_TEXTURE)
        e[key + '_caption'] = text(f'echo_queue_{key}_caption_v1', caption, (x0 + 4, y0 + 12, x1 - 4, y0 + 44), 18, ACCENT, capacity=24)
        value = (x0 + 6, y0 + 144, x1 - 6, y1 - 12)
        if label:
            e[key + '_text'] = text(f'echo_queue_{key}_text_v1', label, value, 34, WHITE, capacity=capacity)
        if busy:
            e[key + '_busy'] = text(f'echo_queue_{key}_busy_v1', 'BUSY', value, 34, DIM, hidden=True)
        return rect, value, icon_rect

    private_rect, _, _ = tile('private', 0, 'PRIVATE MATCH', 'CREATE', 'lock')
    lobby_rect, _, _ = tile('lobby', 1, 'LOBBY', 'GO', 'house')
    stats_rect, _, _ = tile('stats', 2, 'HOURS TODAY', '0 MINS', 'chart', busy=False, capacity=16)
    tile('throw', 3, 'LAST THROW', '-- M/S', 'speed', busy=False, capacity=16)

    # GRAPHICS: its card stops above HIGH and LOW, which sit side by side under it, as wide as the box;
    # the plugin lights the chosen one.
    gfx_box, _, icon_rect = tile('gfx', 4, 'GRAPHICS', None, 'monitor', busy=False, card_h=136)
    vx0, vy0, vx1, vy1 = gfx_box[0], gfx_box[1] + 146, gfx_box[2], gfx_box[3]
    e['gfx_icon_low'] = sprite('echo_queue_gfx_icon_low_v1', icon_rect, icon_uvs['potato'], WHITE, hidden=True, texture=ICON_TEXTURE)   # LOW: a potato on the screen
    gfx_rects = {}
    mid = (vx0 + vx1) / 2
    for key, label, x0, x1 in (('gfx_high', 'HIGH', vx0, mid - 3), ('gfx_low', 'LOW', mid + 3, vx1)):
        rect = (x0, vy0, x1, vy1)
        w, h = x1 - x0, vy1 - vy0
        sprite(f'echo_queue_{key}_base_v1', rect, CARD_UV, WHITE)
        e[key] = sprite(f'echo_queue_{key}_box_v1', rect, CARD_ACTIVE_UV, WHITE, hidden=True)
        sprite(f'echo_queue_{key}_fill_v1', (4, 4, w - 4, h - 4), ROW_FILL_UV, FIND_BORDER, parent=e[key], texture=ROW_ATLAS)
        e[key + '_text'] = text(f'echo_queue_{key}_text_v1', label, rect, 26, WHITE)
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
        e[f'key_{n}_text'] = text(f'echo_queue_key_{n}_text_v1', label, rect, size, WHITE)
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

    # The SFX page: its own tab, eight buttons that each press their own keybind (for soundboards).
    # EDIT, or tapping an empty one, picks the keybind on the keyboard above.
    group = e['sound_page'] = sprite('echo_queue_sound_page_v1', (PAGE_X, PAGE_Y, PAGE_X + PAGE_W, PAGE_Y + PAGE_H),
                                     screen_uv((PAGE_X, PAGE_Y, PAGE_X + PAGE_W, PAGE_Y + PAGE_H)), WHITE, parent=-1, hidden=True)
    sprite('echo_queue_sound_card_v1', CARD, CARD_UV, WHITE)
    e['sound_status'] = text('echo_queue_sound_status_v1', 'TAP A SOUND TO PRESS ITS KEY', (44, 20, 740, 98), 28, TEXT, align=0, capacity=96)
    _, e['sound_edit_text'] = lobby_row('echo_queue_sound_edit_row_v1', SOUND_EDIT, CLIP_FILL, CLIP_BORDER, 'EDIT', size=36)
    sound_rects = []
    for i in range(SOUND_PADS):
        x0 = SOUND_PAD_ORIGIN[0] + i % 4 * (SOUND_PAD_W + SOUND_PAD_GAP)
        y0 = SOUND_PAD_ORIGIN[1] + i // 4 * (SOUND_PAD_H + SOUND_PAD_GAP)
        rect = (x0, y0, x0 + SOUND_PAD_W, y0 + SOUND_PAD_H)
        box = sprite(f'echo_queue_sound_{i}_box_v1', rect, CARD_ACTIVE_UV, WHITE)
        sprite(f'echo_queue_sound_{i}_fill_v1', (6, 6, SOUND_PAD_W - 6, SOUND_PAD_H - 6), ROW_FILL_UV, (*FIND_FILL[:3], .55), parent=box, texture=ROW_ATLAS)
        e[f'sound_caption_{i}'] = text(f'echo_queue_sound_{i}_caption_v1', f'SOUND {i + 1}', (x0, y0 + 14, x0 + SOUND_PAD_W, y0 + 64), 24, ACCENT)
        e[f'sound_text_{i}'] = text(f'echo_queue_sound_{i}_text_v1', 'TAP TO SET', (x0 + 6, y0 + 64, x0 + SOUND_PAD_W - 6, y0 + SOUND_PAD_H - 14), 26, WHITE)
        sound_rects.append(rect)

    # The HOLO page (see HOLO_CONTROLS). Built even without the mod (hidden, never shown), so
    # element numbers don't move. Buttons are dark pills; lit (the plugin shows it) a teal pill
    # with a light edge. The field and grey bar are pictures; their touch areas are a grid over
    # them, each with a ring (hidden) at its centre.
    group = e['holo_page'] = sprite('echo_queue_holo_page_v1', (PAGE_X, PAGE_Y, PAGE_X + PAGE_W, PAGE_Y + PAGE_H),
                                    screen_uv((PAGE_X, PAGE_Y, PAGE_X + PAGE_W, PAGE_Y + PAGE_H)), WHITE, parent=-1, hidden=True)
    sprite('echo_queue_holo_card_v1', CARD, CARD_UV, WHITE)
    holo_rects, holo_frames, holo_fills, holo_texts = {}, {}, {}, {}

    def holo_btn(key, rect, words, size=20, shape='holo_pill', words_rgba=WHITE):
        x0, y0, x1, y1 = rect
        holo_rects[key] = rect
        holo_fills[key] = sprite(f'echo_queue_holo_{key}_base_v2', rect, icon_uvs[shape], HOLO_BTN, texture=ICON_TEXTURE)
        lit = holo_frames[key] = sprite(f'echo_queue_holo_{key}_lit_v2', (x0 - 2, y0 - 2, x1 + 2, y1 + 2), icon_uvs[shape], HOLO_EDGE,
                                        hidden=True, texture=ICON_TEXTURE)
        sprite(f'echo_queue_holo_{key}_lit_fill_v2', (4, 4, x1 - x0, y1 - y0), icon_uvs[shape], HOLO_LIT, parent=lit, texture=ICON_TEXTURE)
        if words:
            holo_texts[key] = text(f'echo_queue_holo_{key}_text_v2', words, rect, size, words_rgba)

    # left: which one, the preview, HOLOGRAM BLUE
    text('echo_queue_holo_editing_v2', 'EDITING', (24, 20, 232, 42), 15, DIM)
    for b, name in enumerate(HOLO_BAR_LABELS):
        holo_btn(f'edit_{HOLO_BARS[b]}', (24, 46 + b * 56, 232, 92 + b * 56), name)
    sprite('echo_queue_holo_preview_edge_v2', (54, 218, 202, 366), icon_uvs['dot'], (.92, .94, .96, 1), texture=ICON_TEXTURE)
    e['holo_preview'] = sprite('echo_queue_holo_preview_v2', (58, 222, 198, 362), icon_uvs['dot'], (.19, .75, 1, 1), texture=ICON_TEXTURE)
    e['holo_hex'] = text('echo_queue_holo_hex_v2', '#30BFFF', (24, 370, 232, 396), 22, WHITE, capacity=16)
    holo_btn('reset', (24, 418, 232, 464), 'HOLOGRAM BLUE', 19, words_rgba=(.19, .75, 1, 1))

    # middle: the field, the grey bar, the - / + rows
    fx0, fy0, fx1, fy1 = HOLO_FIELD
    sprite('echo_queue_holo_field_v2', HOLO_FIELD, icon_uvs['holo_field'], WHITE, texture=ICON_TEXTURE)
    sprite('echo_queue_holo_grey_v2', HOLO_GREY_BAR, icon_uvs['holo_grey'], WHITE, texture=ICON_TEXTURE)

    def ring(key, rect):
        x0, y0, x1, y1 = rect
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        holo_rects[key] = rect
        holo_frames[key] = sprite(f'echo_queue_holo_{key}_ring_v2', (cx - 14, cy - 14, cx + 14, cy + 14), icon_uvs['holo_ring'], WHITE,
                                  hidden=True, texture=ICON_TEXTURE)

    cw, rh = (fx1 - fx0) / HOLO_FIELD_COLS, (fy1 - fy0) / HOLO_FIELD_ROWS
    for r in range(HOLO_FIELD_ROWS):
        for c in range(HOLO_FIELD_COLS):
            ring(f'field_{r}_{c}', (fx0 + c * cw, fy0 + r * rh, fx0 + (c + 1) * cw, fy0 + (r + 1) * rh))
    gx0, gy0, gx1, gy1 = HOLO_GREY_BAR
    gw = (gx1 - gx0) / HOLO_GREYS
    for i in range(HOLO_GREYS):
        ring(f'grey_{i}', (gx0 + i * gw, gy0 - 2, gx0 + (i + 1) * gw, gy1 + 2))
    y0, y1 = 404, 452
    words = {'hue': ('< COLOUR', 'COLOUR >'), 'light': ('DARKER', 'LIGHTER'), 'sat': ('PALER', 'BOLDER')}
    for i, (key, name) in enumerate(zip(HOLO_FINE, ('NUDGE COLOUR', 'BRIGHTNESS', 'VIVIDNESS'))):
        x = 252 + i * 176
        text(f'echo_queue_holo_{key}_label_v2', name, (x, y0 - 24, x + 168, y0 - 4), 15, ACCENT)
        holo_btn(f'{key}_minus', (x, y0, x + 82, y1), words[key][0], 16, shape='holo_tile')
        holo_btn(f'{key}_plus', (x + 86, y0, x + 168, y1), words[key][1], 16, shape='holo_tile')

    # right: RAINBOW and its speed, ARMS
    holo_btn('rainbow', (792, 22, 976, 118), None, shape='holo_box')
    text('echo_queue_holo_rainbow_text_v2', 'RAINBOW', (792, 30, 976, 80), 26, WHITE)
    sprite('echo_queue_holo_rainbow_strip_v2', (808, 90, 960, 102), icon_uvs['holo_rainbow'], WHITE, texture=ICON_TEXTURE)
    text('echo_queue_holo_speed_caption_v2', 'RAINBOW SPEED', (792, 138, 976, 160), 15, DIM)
    for i in range(HOLO_SPEEDS):
        x = 792 + i * 37.5
        holo_btn(f'speed_{i}', (x, 166, x + 34, 200), str(i + 1), 19, shape='dot')
    text('echo_queue_holo_look_caption_v2', 'ARMS', (792, 234, 976, 256), 15, DIM)
    for i, name in enumerate(('SOLID', 'SEE-THROUGH', 'SOLID HANDS', 'CLEAR HANDS')):
        holo_btn(f'look_{HOLO_LOOKS[i]}', (792, 262 + i * 46, 976, 304 + i * 46), name, 18, shape='holo_pill_short')
    text('echo_queue_holo_look_note_v2', 'CHANGES NEXT TIME', (792, 444, 976, 459), 13, DIM)
    text('echo_queue_holo_look_note2_v2', 'YOU START ECHO', (792, 459, 976, 474), 13, DIM)
    e['holo_status'] = text('echo_queue_holo_status_v2', 'TAP A COLOUR, THEN FINE-TUNE IT WITH THE BUTTONS UNDER IT', (24, 478, 976, 506), 17, DIM, capacity=96)

    # The title strip (a cover over the stock title, PLAY's own title) and the tab, top level.
    group = -1
    title_area = (54, 22, 822, 103)
    cover = sprite('echo_queue_title_cover_v1', title_area, screen_uv(title_area), WHITE, hidden=True)
    header = page.append(panel_template, 'echo_queue_header_background_v1', (60, 40, 210, 108), hidden=True)
    colour(page, header, WHITE)
    title = text('echo_queue_title_v1', 'PLAY', (76, 40, 210, 108), 44, BLACK, align=0, hidden=True, capacity=64)
    # The subtitle on a grey box, long enough for ARENA MATCHMAKING; the wide one while the keyboard's
    # up (its prompts are longer, and the clock is hidden then).
    subtitle_box = page.append(panel_template, 'echo_queue_subtitle_box_v1', (226, 48, 548, 100), hidden=True)
    colour(page, subtitle_box, GREY)
    subtitle_box_wide = page.append(panel_template, 'echo_queue_subtitle_box_wide_v1', (226, 48, 816, 100), hidden=True)
    colour(page, subtitle_box_wide, GREY)
    # PLAY's status (READY, or the queue timer while searching) in the strip's top right corner, left of the stock close button.
    e['timer_caption'] = text('echo_queue_timer_caption_v1', 'STATUS', (580, 48, 702, 100), 18, ACCENT, align=1, hidden=True, capacity=24)
    e['timer'] = text('echo_queue_timer_v1', 'READY', (714, 40, 840, 108), 32, WHITE, align=0, hidden=True, capacity=16)
    subtitle = text('echo_queue_subtitle_v1', 'MATCHMAKING', (242, 40, 810, 108), 30, WHITE, align=0, hidden=True, capacity=64)
    # The clock at the strip's right end, and the 12H / 24H switch next to it (it names the format shown).
    clock = text('echo_queue_clock_v1', '00:00', (560, 40, 722, 108), 36, WHITE, align=1, hidden=True, capacity=16)
    clock_format = sprite('echo_queue_clock_format_v1', CLOCK_FORMAT, CARD_ACTIVE_UV, WHITE, hidden=True)
    clock_format_text = text('echo_queue_clock_format_text_v1', '24H', CLOCK_FORMAT, 26, WHITE, hidden=True, capacity=8)
    tab_icon = sprite('echo_queue_tab_icon_v1', tab_rect, icon_uvs['tab_idle'], WHITE, texture=ICON_TEXTURE)
    tab_selected = sprite('echo_queue_tab_selected_v1', tab_rect, icon_uvs['tab_selected'], WHITE, hidden=True, texture=ICON_TEXTURE)
    sound_tab_icon = sprite('echo_queue_sound_tab_icon_v1', sound_tab_rect, icon_uvs['sound_idle'], WHITE, texture=ICON_TEXTURE)
    sound_tab_selected = sprite('echo_queue_sound_tab_selected_v1', sound_tab_rect, icon_uvs['sound_selected'], WHITE, hidden=True, texture=ICON_TEXTURE)
    holo_tab_icon = sprite('echo_queue_holo_tab_icon_v1', holo_tab_rect, icon_uvs['holo_idle'], WHITE, hidden=not holo, texture=ICON_TEXTURE)
    holo_tab_selected = sprite('echo_queue_holo_tab_selected_v1', holo_tab_rect, icon_uvs['holo_selected'], WHITE, hidden=True, texture=ICON_TEXTURE)

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
    add_button(START_BUTTON, ARENA)
    add_button(COMBAT_BUTTON, COMBAT)
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
    add_button(PLAY_CLOCK_BUTTON, TIME)
    add_button(CLOCK_12_BUTTON, clock_rects['clock_12'])
    add_button(CLOCK_24_BUTTON, clock_rects['clock_24'])
    add_button(UPDATE_BUTTON, UPDATE)
    add_button(SOUND_TAB_BUTTON, sound_tab_rect, on_page=False)
    add_button(SOUND_EDIT_BUTTON, SOUND_EDIT)
    for i, rect in enumerate(sound_rects):
        add_button(sound_button(i), rect)
    if holo:   # without the mod there's nothing to touch: no tab, no segments
        add_button(HOLO_TAB_BUTTON, holo_tab_rect, on_page=False)
        for key in HOLO_CONTROLS:
            add_button(holo_button(key), holo_rects[key])
    moved_rows = {r for t in tabs for r in t['rows']}
    for r, old in enumerate(old_rows):
        assert r in moved_rows or buttons.rows[r] == old

    icon_cpu, icon_gpu = texture_pair(m, ICON_W, ICON_H, icon_pixels)
    resources = [(CV, ROOT_CANVAS, root.serialize()), (CV, NAV_CANVAS, nav.serialize()), (CV, OVERLAY, page.serialize()),
                 (BUTTONS, LEVEL, buttons.serialize()), (TEX, ICON_TEXTURE, icon_cpu), (TEX_GPU, ICON_TEXTURE, icon_gpu)]
    e.update(page=page_group, cover=cover, header=header, title=title, subtitle=subtitle, subtitle_box=subtitle_box,
             subtitle_box_wide=subtitle_box_wide, clock=clock, clock_format=clock_format,
             clock_format_text=clock_format_text, tab_icon=tab_icon, tab_selected=tab_selected,
             sound_tab_icon=sound_tab_icon, sound_tab_selected=sound_tab_selected,
             holo_tab_icon=holo_tab_icon, holo_tab_selected=holo_tab_selected)

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
                  overlay_elements=len(page.elements), holo=int(holo))
    return dict(resources=resources, layout=layout, notes=notes, details=details, page=e, tabs=len(tabs) + ours,
                holo_frames=[holo_frames[k] for k in HOLO_CONTROLS],
                fine_fills=[holo_fills[f'{k}_{x}'] for k in HOLO_FINE for x in ('minus', 'plus')],
                fine_texts=[holo_texts[f'{k}_{x}'] for k in HOLO_FINE for x in ('minus', 'plus')])


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
    x0, _, x1, _ = Canvas.rect(overlay.elements[built['page']['sound_tab_icon']])
    tabs.append(((x0 + x1) / 2, dict(describe(overlay, [overlay.elements[built['page']['sound_tab_icon']]], ours=True), sound=True)))
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
    lines.append(f"holo={layout.get('holo', 0)}")   # the HOLO tab is in (the hologram chassis mod is installed)
    return '\r\n'.join(lines) + '\r\n'


def page_header(built):
    """src/generated/queue_tab.h: the button names and the overlay's element indices."""
    e = built['page']
    return ('// Generated by installer/tablet.py -- do not edit.\n#pragma once\n#include <cstdint>\n'
            f'constexpr uint64_t QUEUE_TAB=0x{TAB_BUTTON:016x}, QUEUE_START=0x{START_BUTTON:016x}, QUEUE_COMBAT=0x{COMBAT_BUTTON:016x}, '
            f'QUEUE_CANCEL=0x{CANCEL_BUTTON:016x}, QUEUE_LOBBY=0x{LOBBY_BUTTON:016x}, QUEUE_PRIVATE=0x{PRIVATE_BUTTON:016x}, '
            f'QUEUE_STATS=0x{STATS_BUTTON:016x}, QUEUE_GFX_HIGH=0x{GFX_HIGH_BUTTON:016x}, QUEUE_GFX_LOW=0x{GFX_LOW_BUTTON:016x}, QUEUE_CLIP=0x{CLIP_BUTTON:016x}, QUEUE_CLIP_KEY=0x{CLIP_KEY_BUTTON:016x}, QUEUE_CLOCK=0x{CLOCK_BUTTON:016x}, '
            f'QUEUE_SOUND_TAB=0x{SOUND_TAB_BUTTON:016x}, QUEUE_SOUND_EDIT=0x{SOUND_EDIT_BUTTON:016x}, QUEUE_PLAY_CLOCK=0x{PLAY_CLOCK_BUTTON:016x}, '
            f'QUEUE_CLOCK_12=0x{CLOCK_12_BUTTON:016x}, QUEUE_CLOCK_24=0x{CLOCK_24_BUTTON:016x}, QUEUE_UPDATE=0x{UPDATE_BUTTON:016x};\n'
            'constexpr uint64_t QUEUE_SOUND_BUTTONS[] = {' + ', '.join(f'0x{sound_button(i):016x}' for i in range(SOUND_PADS)) + '};\n'
            'constexpr unsigned QUEUE_SOUND_TEXTS[] = {' + ', '.join(str(e[f'sound_text_{i}']) for i in range(SOUND_PADS)) + '};\n'
            'constexpr unsigned QUEUE_SOUND_CAPTIONS[] = {' + ', '.join(str(e[f'sound_caption_{i}']) for i in range(SOUND_PADS)) + '};\n'
            f"constexpr unsigned QUEUE_OVERLAY_ELEMENTS={built['layout']['overlay_elements']}, QUEUE_LAYOUT_VERSION={built['layout']['version']};\n"
            + ''.join(f'constexpr unsigned QUEUE_PAGE_{k.upper()}={v};\n' for k, v in e.items()
                      if not k.startswith('key_') or k in ('key_no_clip_app_text', 'key_ctrl_text', 'key_shift_text', 'key_alt_text'))
            + f'constexpr uint64_t QUEUE_HOLO_TAB=0x{HOLO_TAB_BUTTON:016x};\n'
            + f'constexpr unsigned HOLO_BAR_COUNT={len(HOLO_BARS)}, HOLO_SPEEDS={HOLO_SPEEDS}, HOLO_LOOKS={len(HOLO_LOOKS)}, HOLO_FINE={len(HOLO_FINE)}, HOLO_CONTROL_COUNT={len(HOLO_CONTROLS)};\n'
            + f"constexpr unsigned HOLO_RESET={HOLO_CONTROLS.index('reset')}, HOLO_RAINBOW={HOLO_CONTROLS.index('rainbow')}, "
            + f"HOLO_SPEED_FIRST={HOLO_CONTROLS.index('speed_0')}, HOLO_LOOK_FIRST={HOLO_CONTROLS.index('look_solid')}, HOLO_FINE_FIRST={HOLO_CONTROLS.index('hue_minus')};\n"
            + f"constexpr unsigned HOLO_FIELD_FIRST={HOLO_CONTROLS.index('field_0_0')}, HOLO_FIELD_COLS={HOLO_FIELD_COLS}, HOLO_FIELD_ROWS={HOLO_FIELD_ROWS}, "
            + f"HOLO_GREY_FIRST={HOLO_CONTROLS.index('grey_0')}, HOLO_GREYS={HOLO_GREYS};\n"
            + f'constexpr float HOLO_FIELD_TOP={HOLO_FIELD_TOP}f, HOLO_FIELD_SPAN={HOLO_FIELD_SPAN}f;\n'
            + 'constexpr uint64_t QUEUE_HOLO_BUTTONS[] = {' + ', '.join(f'0x{holo_button(k):016x}' for k in HOLO_CONTROLS) + '};\n'
            + 'constexpr unsigned QUEUE_HOLO_FRAMES[] = {' + ', '.join(map(str, built['holo_frames'])) + '};\n'
            + 'constexpr unsigned QUEUE_HOLO_FINE_FILLS[] = {' + ', '.join(map(str, built['fine_fills'])) + '};\n'
            + 'constexpr unsigned QUEUE_HOLO_FINE_TEXTS[] = {' + ', '.join(map(str, built['fine_texts'])) + '};\n'
            + 'constexpr const char* HOLO_BAR_LABELS[] = {' + ', '.join(f'"{l}"' for l in HOLO_BAR_LABELS) + '};\n'
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
