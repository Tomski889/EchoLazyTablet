"""GRAPHICS HIGH / LOW: the game data and settings the tablet's buttons switch between.

LOW is "potato" graphics. Echo's world textures stream their sharp mips at run time and keep a
small mip tail (128 px) in the texture header: each streamed texture becomes a plain resident
texture holding only that tail cut to 4 px, big resident ones lose their top mips the same way,
all written as one new package. UI textures stay as they are, player and cosmetic textures (team
colours, banners) are cut to 128 px only, and the background effects outside the arena and lobby
are blanked. Echo's own settings go to their lowest (no shadows; render resolution left alone).

The stock packages are never written. Both manifests stay next to the game's (<manifest>.potato-high
and .potato-low) and both sets of settings in plugins\\EchoLazyTablet.graphics.ini; the plugin
copies one over the live manifest when a button is pressed (it takes a restart). Same files and
names as the EchoPotato tool this came from, so either can undo the other.
"""
import json
import os
import struct
from pathlib import Path

import echodata as E

TEX, TEX_GPU = E.typesym('CGTextureResource'), E.typesym('CGTextureResource', True)
CANVAS = E.typesym('CUICanvasResource')
REWARDS = E.typesym('CR15NetRewardItemCR')
STREAM = 0xae49fad43254367a                                         # streamed texture mips (large, no references)
BC8 = {70, 71, 72, 79, 80, 81}                                      # BC1, BC4: 8 bytes per 4x4 block
BC16 = {73, 74, 75, 76, 77, 78, 82, 83, 84, 94, 95, 96, 97, 98, 99}  # BC2/3/5/6H/7: 16 bytes
WORLD_SIZE, PLAYER_SIZE = 4, 128
# Textures of the background effects outside the arena and lobby (particle effects 0xdc8adf1c3054b91f:
# the blue band; 0x47c3dcb5363e6f3f, 0x7519175b4e041801, 0x1842386e1a3a999d: the red streaks). Nothing
# else in the game uses them (24 Oct 2026).
HIDE_IN_LOW = {0x064682fd40885ac8, 0x488195e980151a22}

POTATO_GRAPHICS = {'msaa': 0, 'temporalaa': False, 'sharpening': 0.0, 'adaptiveresolution': False}
POTATO_QUALITY = {'shadows': 0, 'shadowresolution': 256, 'cascaderesolution': 512, 'shadowmsaa': 0, 'cascadedistance': 0.25,
                  'fx': 0, 'lights': 0, 'meshes': 0, 'textures': 0, 'bloom': False, 'volumetrics': False}


class GraphicsError(Exception):
    """LOW graphics couldn't be set up; the message says why, in plain words."""


def paths(game):
    """(data folder, live manifest, HIGH copy, LOW copy, record) for the game folder."""
    data = Path(game) / '_data' / '5932408047' / 'rad15' / 'win10'
    man = data / 'manifests' / E.MID
    return data, man, man.with_name(E.MID + '.potato-high'), man.with_name(E.MID + '.potato-low'), man.with_name(E.MID + '.potato.json')


def settings_path():
    return Path(os.environ.get('LOCALAPPDATA', '')) / 'rad' / 'loneecho' / 'settings_mp_v2.json'


def ini_path(game):
    return Path(game) / 'bin' / 'win10' / 'plugins' / 'EchoLazyTablet.graphics.ini'


def write_atomic(path, data):
    tmp = path.with_name(path.name + '.potato-tmp')
    tmp.write_bytes(data)
    os.replace(tmp, path)


def mip_bytes(fmt, w, h):
    return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * (8 if fmt in BC8 else 16)


def chain_bytes(fmt, w, h, mips):
    return sum(mip_bytes(fmt, max(1, w >> i), max(1, h >> i)) for i in range(mips))


def cut_mips(dds, fmt, w, h, mips, pitch, max_size):
    """Drops a BC DDS's top mips until it is at most max_size. -> (dds, w, h, mips, pitch)."""
    drop = 0
    while fmt in BC8 | BC16 and mips - drop > 1 and max(w >> drop, h >> drop) > max_size:
        drop += 1
    if not drop:
        return dds, w, h, mips, pitch
    cut = chain_bytes(fmt, w, h, drop)
    w, h, mips = max(1, w >> drop), max(1, h >> drop), mips - drop
    pitch = mip_bytes(fmt, w, h)
    dds = dds[:148] + dds[148 + cut:]
    struct.pack_into('<III', dds, 12, h, w, pitch)
    struct.pack_into('<I', dds, 28, mips)
    return dds, w, h, mips, pitch


def shrink(cpu, gpu, max_size):
    """A texture's (header, GPU data) -> smaller (resident header, DDS), or None to leave it."""
    if len(cpu) < 0x100:
        return None
    kind, w, h, mips, d0, d4, fmt, srgb, e0, e4, tw, th, tmips, size, pitch, fc = struct.unpack_from('<16I', cpu, 0xc0)
    if kind == 0 and len(cpu) > 0x100 + 148:
        dds = bytearray(cpu[0x100:0x100 + size])
    elif kind == 1 and len(cpu) == 0x100 and d0 == 1 and mips > 1 and max(w, h) >= 256 and fmt in BC8 | BC16:
        dds = bytearray(gpu or b'')
        if (tw, th, tmips) != (w, h, mips) or len(dds) != 148 + chain_bytes(fmt, w, h, mips):
            return None
    else:
        return None
    if len(dds) != size or dds[:4] != b'DDS ' or dds[84:88] != b'DX10':
        return None
    if struct.unpack_from('<IIII', dds, 12) != (th, tw, pitch, 1) or struct.unpack_from('<I', dds, 28)[0] != tmips \
            or struct.unpack_from('<I', dds, 128)[0] != fmt:
        return None
    dds, tw, th, tmips, pitch = cut_mips(dds, fmt, tw, th, tmips, pitch, max_size)
    if kind == 1 and tw == w:
        return None
    head = bytearray(cpu[:0x100])
    struct.pack_into('<16I', head, 0xc0, 1, tw, th, tmips, d0, d4, fmt, srgb, e0, e4, tw, th, tmips, len(dds), pitch, fc)
    return bytes(head), bytes(dds)


class References:
    """Which resource names each resource group's data mentions (8-byte values at 4-byte steps)."""

    def __init__(self, m):
        self.m = m
        self.byname = {}
        for e in m.entries:
            self.byname.setdefault(e[1], []).append(e)
        self.cache = {}

    def entry(self, e):
        if len(self.m._cache) > 8:
            self.m._cache.clear()
        blob, out = self.m.get(e[0], e[1]), set()
        for start in (0, 4):
            chunk = blob[start:]
            out.update(v for (v,) in struct.iter_unpack('<Q', chunk[:len(chunk) // 8 * 8]) if v in self.byname)
        return out

    def of(self, name):
        if name not in self.cache:
            self.cache[name] = set().union(*(self.entry(e) for e in self.byname.get(name, []) if e[0] not in (TEX, TEX_GPU, STREAM)))
        return self.cache[name]


def ui_textures(m, names):
    """Textures any UI canvas refers to (tablet, menus, HUD): left as they are so text stays readable."""
    used = set()
    for e in sorted((e for e in m.entries if e[0] == CANVAS), key=lambda e: e[2] & 0xFFFFFFFF):
        if len(m._cache) > 8:
            m._cache.clear()
        blob = m.get(CANVAS, e[1])
        used.update(v for (v,) in struct.iter_unpack('<Q', blob[:len(blob) // 8 * 8]) if v in names)
    return used


def player_textures(m, game, names):
    """Textures of the player models (suits, bracers, backpacks, their tints), found by following
    references out of every customisation model, and of every cosmetic reward item (banners,
    emblems and decals, patterns, emotes, tags), from the reward table and one step past it."""
    models = json.loads((Path(game) / 'sourcedb' / 'rad15' / 'json' / 'r14' / 'multiplayer' / 'customization_models.json').read_text())
    refs = References(m)
    seen, used = set(), set()
    frontier = {E.sym(s) for v in models.values() for x in (v.values() if isinstance(v, dict) else [v]) for s in x}
    while frontier:   # textures are collected, not followed
        nxt = set()
        for n in frontier:
            seen.add(n)
            r = refs.of(n)
            used |= r & names
            nxt |= r - names
        frontier = nxt - seen
    for e in (e for e in m.entries if e[0] == REWARDS):
        for n in refs.entry(e):
            used |= {n} & names or refs.of(n) & names
    return used


def apply(game, log=print):
    """Builds LOW from the live manifest (which must be the game's own: undo() first) and makes it
    live, with Echo's settings at their lowest. -> number of textures made smaller."""
    data, man, high_path, low_path, record = paths(game)
    if record.exists():
        raise GraphicsError('Low graphics are already set up; take them out first.')
    base = man.read_bytes()
    m = E.Manifest(base, data)
    textures = {e[1]: e for e in m.entries if e[0] == TEX}
    gpus = {e[1] for e in m.entries if e[0] == TEX_GPU}
    spare = ui_textures(m, set(textures))
    players = player_textures(m, game, set(textures))
    resources = []
    for name in sorted(set(textures) - spare, key=lambda n: textures[n][2] & 0xFFFFFFFF):
        if len(m._cache) > 8:
            m._cache.clear()
        cpu = m.get(TEX, name)
        gpu = m.get(TEX_GPU, name) if len(cpu) == 0x100 and name in gpus else None
        out = shrink(cpu, gpu, PLAYER_SIZE if name in players else WORLD_SIZE)
        if out is None:
            continue
        if name in HIDE_IN_LOW:   # all-zero blocks: black and fully transparent (BC3), so the effect draws nothing
            out = (out[0], out[1][:148] + bytes(len(out[1]) - 148))
        resources += [(TEX, name, out[0]), (TEX_GPU, name, out[1])]
    package = data / 'packages' / f'{E.MID}_{m.packages}'
    if package.exists():
        raise GraphicsError(f'The game data already has a package file {package.name} that is not in use; '
                            'low graphics were not set up so as not to overwrite it.')
    number, blob = m.add_package(resources)
    low = m.serialize()
    write_atomic(package, blob)
    write_atomic(high_path, base)
    write_atomic(low_path, low)
    write_atomic(man, low)
    record.write_text(json.dumps({'package': number, 'max_size': WORLD_SIZE, 'player_size': PLAYER_SIZE, 'textures': len(resources) // 2}))
    apply_settings(game)
    log(f'Low graphics ready: {len(resources) // 2} textures made smaller (package _{number}, {len(blob) / 1e6:.0f} MB).')
    return len(resources) // 2


def read_ini(path):
    sections, current = {}, None
    for line in path.read_text().splitlines() if path.exists() else []:
        line = line.strip()
        if line.startswith('['):
            current = sections.setdefault(line.strip('[]'), {})
        elif '=' in line and current is not None:
            k, v = line.split('=', 1)
            current[k] = json.loads(v)
    return sections


def set_graphics(values):
    path = settings_path()
    if not path.exists():   # Echo hasn't run on this PC yet: it makes the file, and the plugin fills it in
        return
    settings = json.loads(path.read_text())
    g = settings.get('graphics', {})
    for k, v in values.items():
        (g if k in POTATO_GRAPHICS else g.setdefault('quality', {}))[k] = v
    write_atomic(path, json.dumps(settings, indent=4).encode())


def current_values():
    path = settings_path()
    g = json.loads(path.read_text()).get('graphics', {}) if path.exists() else {}
    q = g.get('quality', {})
    defaults = {'msaa': 0, 'temporalaa': False, 'sharpening': 2.0, 'adaptiveresolution': False, 'shadows': 1, 'shadowresolution': 768,
                'cascaderesolution': 2048, 'shadowmsaa': 2, 'cascadedistance': 1.0, 'fx': 1, 'lights': 1, 'meshes': 1, 'textures': 1,
                'bloom': False, 'volumetrics': False}   # Echo's own, as a fresh settings file has them
    return {k: g.get(k, defaults[k]) for k in POTATO_GRAPHICS} | {k: q.get(k, defaults[k]) for k in POTATO_QUALITY}


def apply_settings(game):
    """The ini with HIGH (the player's settings as they were, kept from the first time) and LOW, and LOW
    put in Echo's settings."""
    ini = ini_path(game)
    high = read_ini(ini).get('high') or current_values()
    lines = ["; Written by the EchoLazyTablet installer: what the tablet's GRAPHICS HIGH / LOW buttons set in settings_mp_v2.json."]
    for name, values in (('high', high), ('low', POTATO_GRAPHICS | POTATO_QUALITY)):
        lines += [f'[{name}]'] + [f'{k}={json.dumps(v)}' for k, v in values.items()]
    write_atomic(ini, ('\r\n'.join(lines) + '\r\n').encode())
    set_graphics(POTATO_GRAPHICS | POTATO_QUALITY)


def is_low(game):
    data, man, high_path, low_path, record = paths(game)
    return low_path.exists() and man.exists() and man.read_bytes() == low_path.read_bytes()


def use_high(game):
    """HIGH made live (manifest and settings), LOW kept ready for the button."""
    data, man, high_path, low_path, record = paths(game)
    write_atomic(man, high_path.read_bytes())
    high = read_ini(ini_path(game)).get('high')
    if high:
        set_graphics(high)


def mode(game):
    """'low' or 'high' as the game data is now, or None (LOW not set up, or the data changed since)."""
    data, man, high_path, low_path, record = paths(game)
    if not (high_path.exists() and low_path.exists() and man.exists()):
        return None
    live = man.read_bytes()
    return 'low' if live == low_path.read_bytes() else 'high' if live == high_path.read_bytes() else None


def switch(game, low):
    """What the tablet's GRAPHICS buttons do, from the installer: the manifest, Echo's settings and the
    plugin's [graphics] mode. Takes effect when Echo next starts."""
    if mode(game) is None:
        raise GraphicsError('Low graphics are not set up on this game (or another tool changed the game data): run Repair.')
    data, man, high_path, low_path, record = paths(game)
    write_atomic(man, (low_path if low else high_path).read_bytes())
    values = read_ini(ini_path(game)).get('low' if low else 'high')
    if values:
        set_graphics(values)
    import ctypes
    ctypes.windll.kernel32.WritePrivateProfileStringW('graphics', 'mode', 'low' if low else 'high',
                                                      str(Path(game) / 'bin' / 'win10' / 'plugins' / 'EchoLazyTablet.ini'))


def undo(game, log=print, keep_ini=False):
    """The game's own manifest (HIGH) back and the LOW files gone; Echo's settings back to HIGH."""
    data, man, high_path, low_path, record = paths(game)
    if not high_path.exists():
        return False
    info = json.loads(record.read_text()) if record.exists() else {}
    write_atomic(man, high_path.read_bytes())
    if 'package' in info:
        (data / 'packages' / f'{E.MID}_{info["package"]}').unlink(missing_ok=True)
    for path in (record, high_path, low_path, man.with_name(E.MID + '.potato-backup')):
        path.unlink(missing_ok=True)
    high = read_ini(ini_path(game)).get('high')
    if high:
        set_graphics(high)
    if not keep_ini:
        ini_path(game).unlink(missing_ok=True)
    log('Took out the low graphics files; normal graphics are back.')
    return True
