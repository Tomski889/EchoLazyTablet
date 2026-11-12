"""EchoLazyTablet's icon glyphs, drawn in the tablet's flat white style (4x supersampled).

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


def lock(size):
    """PRIVATE MATCH: a padlock."""
    s = 4
    w = size * s
    img = Image.new('RGBA', (w, w), CLEAR)
    d = ImageDraw.Draw(img)
    d.ellipse((w * .26, w * .06, w * .74, w * .54), fill=WHITE)                 # shackle
    d.ellipse((w * .36, w * .16, w * .64, w * .44), fill=CLEAR)
    d.rectangle((w * .36, w * .3, w * .64, w * .46), fill=CLEAR)
    d.rectangle((w * .26, w * .3, w * .36, w * .46), fill=WHITE)
    d.rectangle((w * .64, w * .3, w * .74, w * .46), fill=WHITE)
    d.rounded_rectangle((w * .14, w * .44, w * .86, w * .94), radius=w * .07, fill=WHITE)   # body
    d.ellipse((w * .43, w * .56, w * .57, w * .7), fill=CLEAR)                  # keyhole
    d.rectangle((w * .47, w * .64, w * .53, w * .82), fill=CLEAR)
    return img.resize((size, size), Image.LANCZOS)


def chart(size):
    """The stats box: three rising bars on a base line."""
    s = 4
    w = size * s
    img = Image.new('RGBA', (w, w), CLEAR)
    d = ImageDraw.Draw(img)
    for x, top in ((.12, .55), (.4, .32), (.68, .1)):
        d.rounded_rectangle((w * x, w * top, w * (x + .2), w * .84), radius=w * .03, fill=WHITE)
    d.rectangle((w * .04, w * .88, w * .96, w * .95), fill=WHITE)
    return img.resize((size, size), Image.LANCZOS)


def speed(size):
    """LAST THROW: a disc flying right, with speed lines behind it."""
    s = 4
    w = size * s
    img = Image.new('RGBA', (w, w), CLEAR)
    d = ImageDraw.Draw(img)
    c, r = (w * .7, w / 2), w * .27
    d.ellipse((c[0] - r, c[1] - r, c[0] + r, c[1] + r), fill=WHITE)
    d.ellipse((c[0] - r * .62, c[1] - r * .62, c[0] + r * .62, c[1] + r * .62), fill=CLEAR)
    d.ellipse((c[0] - r * .3, c[1] - r * .3, c[0] + r * .3, c[1] + r * .3), fill=WHITE)
    for y, x0 in ((.3, .14), (.5, .02), (.7, .14)):
        d.rounded_rectangle((w * x0, w * (y - .045), w * .38, w * (y + .045)), radius=w * .045, fill=WHITE)
    return img.resize((size, size), Image.LANCZOS)


def monitor(size):
    """GRAPHICS: a screen on a stand."""
    s = 4
    w = size * s
    img = Image.new('RGBA', (w, w), CLEAR)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((w * .04, w * .12, w * .96, w * .72), radius=w * .06, fill=WHITE)
    d.rectangle((w * .13, w * .21, w * .87, w * .63), fill=CLEAR)
    d.polygon([(w * .2, w * .58), (w * .42, w * .36), (w * .56, w * .5), (w * .66, w * .42), (w * .8, w * .58)], fill=WHITE)
    d.rectangle((w * .44, w * .72, w * .56, w * .84), fill=WHITE)
    d.rounded_rectangle((w * .26, w * .84, w * .74, w * .92), radius=w * .03, fill=WHITE)
    return img.resize((size, size), Image.LANCZOS)


def potato(size):
    """GRAPHICS while LOW is chosen: the same screen with a potato on it."""
    s = 4
    w = size * s
    img = Image.new('RGBA', (w, w), CLEAR)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((w * .04, w * .12, w * .96, w * .72), radius=w * .06, fill=WHITE)
    d.rectangle((w * .13, w * .21, w * .87, w * .63), fill=CLEAR)
    spud = Image.new('RGBA', (w, w), CLEAR)            # a lumpy tan oval, tilted, with darker eyes
    p = ImageDraw.Draw(spud)
    tan, shade, eye = (206, 160, 98, 255), (168, 122, 66, 255), (110, 74, 36, 255)
    for fill, grow in ((shade, 0), (tan, -.02)):          # a darker rim (both lumps), then the potato inside it
        for x0, y0, x1, y1 in ((.17, .33, .6, .55), (.45, .29, .84, .52)):
            p.ellipse((w * (x0 - grow), w * (y0 - grow), w * (x1 + grow), w * (y1 + grow)), fill=fill)
    for x, y in ((.3, .43), (.55, .46), (.7, .38)):
        p.arc((w * (x - .035), w * (y - .02), w * (x + .035), w * (y + .03)), 200, 340, fill=eye, width=round(w * .014))
    img.alpha_composite(spud.rotate(-12, center=(w / 2, w * .42), resample=Image.BICUBIC))
    d.rectangle((w * .44, w * .72, w * .56, w * .84), fill=WHITE)
    d.rounded_rectangle((w * .26, w * .84, w * .74, w * .92), radius=w * .03, fill=WHITE)
    return img.resize((size, size), Image.LANCZOS)


def speaker(size):
    s = 4
    w = size * s
    img = Image.new('RGBA', (w, w), CLEAR)
    d = ImageDraw.Draw(img)
    d.polygon([(w * .06, w * .36), (w * .24, w * .36), (w * .46, w * .14), (w * .46, w * .86), (w * .24, w * .64), (w * .06, w * .64)], fill=WHITE)
    c = w * .46
    for r_out, r_in in ((.2, .12), (.38, .3)):   # the sound: two arcs, clear of the cone
        ring = Image.new('RGBA', (w, w), CLEAR)
        a = ImageDraw.Draw(ring)
        box = lambda r: (c - w * r, w / 2 - w * r, c + w * r, w / 2 + w * r)
        a.pieslice(box(r_out), -50, 50, fill=WHITE)
        a.ellipse(box(r_in), fill=CLEAR)
        img.alpha_composite(ring)
    return img.resize((size, size), Image.LANCZOS)


def palette(size):
    """The HOLO tab: six dots round a ring, like a colour wheel, with a dot in the middle."""
    s = 4
    w = size * s
    img = Image.new('RGBA', (w, w), CLEAR)
    d = ImageDraw.Draw(img)
    c, ring, r = w / 2, w * .34, w * .12
    for k in range(6):
        a = math.radians(k * 60 - 90)
        x, y = c + ring * math.cos(a), c + ring * math.sin(a)
        d.ellipse((x - r, y - r, x + r, y + r), fill=WHITE)
    d.ellipse((c - r * .8, c - r * .8, c + r * .8, c + r * .8), fill=WHITE)
    return img.resize((size, size), Image.LANCZOS)
