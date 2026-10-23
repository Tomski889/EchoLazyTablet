"""EchoQueue's icon glyphs, drawn in the tablet's flat white style (4x supersampled).

disc():   the Echo Arena disc seen from above, after the in-game disc: four thick rim segments,
          a thin inner ring split the same way, two curved arcs round the hub, a plain centre.
house():  the lobby: a house outline with a door, after the stock home tab glyph.
"""
import math
from PIL import Image, ImageDraw

WHITE = (255, 255, 255, 255)
CLEAR = (0, 0, 0, 0)
GAPS = (-62, 28, 118, 208)   # where the rim is split (degrees, clockwise from 3 o'clock)


def _ring(d, c, r_out, r_in):
    d.ellipse((c - r_out, c - r_out, c + r_out, c + r_out), fill=WHITE)
    d.ellipse((c - r_in, c - r_in, c + r_in, c + r_in), fill=CLEAR)


def _cut(d, c, length, angle, width):
    """Clear a straight gap of `width` from the centre outwards at `angle`."""
    a = math.radians(angle)
    dx, dy = math.cos(a), math.sin(a)
    px, py = -dy * width / 2, dx * width / 2
    d.polygon([(c + px, c + py), (c - px, c - py),
               (c + dx * length - px, c + dy * length - py), (c + dx * length + px, c + dy * length + py)], fill=CLEAR)


def _arc(d, c, r_out, r_in, start, end):
    d.pieslice((c - r_out, c - r_out, c + r_out, c + r_out), start, end, fill=WHITE)
    d.ellipse((c - r_in, c - r_in, c + r_in, c + r_in), fill=CLEAR)


def disc(size):
    s = 4
    w = size * s
    img = Image.new('RGBA', (w, w), CLEAR)
    d = ImageDraw.Draw(img)
    c = w / 2
    _ring(d, c, w * .49, w * .375)
    _ring(d, c, w * .345, w * .305)
    for g in GAPS:
        _cut(d, c, w, g, w * .075)
    arcs = Image.new('RGBA', (w, w), CLEAR)   # own layer, so the rim's gaps don't cut them
    a = ImageDraw.Draw(arcs)
    _arc(a, c, w * .235, w * .19, -95, -5)
    _arc(a, c, w * .235, w * .19, 85, 175)
    img.alpha_composite(arcs)
    r_hub = w * .13
    d.ellipse((c - r_hub, c - r_hub, c + r_hub, c + r_hub), fill=WHITE)
    return img.resize((size, size), Image.LANCZOS)


def house(size):
    s = 4
    w = size * s
    img = Image.new('RGBA', (w, w), CLEAR)
    d = ImageDraw.Draw(img)
    t = w * .1                                   # stroke
    left, right, eave, base, peak = w * .14, w * .86, w * .46, w * .9, w * .1
    d.polygon([(w / 2, peak), (w * .02, eave + t * .2), (w * .98, eave + t * .2)], fill=WHITE)   # roof
    d.rectangle((left, eave, right, base), fill=WHITE)                                           # walls
    inner = [(w / 2, peak + t * 1.9), (w * .02 + t * 1.75, eave + t * .2), (w * .98 - t * 1.75, eave + t * .2)]
    d.polygon(inner, fill=CLEAR)
    d.rectangle((left + t, eave, right - t, base - t), fill=CLEAR)
    d.rectangle((w * .41, w * .58, w * .59, base), fill=WHITE)                                   # door
    return img.resize((size, size), Image.LANCZOS)
