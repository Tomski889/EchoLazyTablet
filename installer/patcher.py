"""Install, repair and remove EchoQueue, and report what else is on the hand tablet.

EchoQueue's game data is one extra package plus the manifest entries pointing at it. What each of
those entries pointed at before (stock data, or another mod's version of the same tablet page) is
kept in manifests/48037dc70b0ecab2.echoqueue.json, so taking EchoQueue out puts exactly that back
and leaves every other mod's changes alone. Install and Repair always start by taking out any
EchoQueue already there, then rebuild the tab on top of the tablet as it is now.

Before any change the manifest is copied to manifests/48037dc70b0ecab2.echoqueue-backup. New files
are written beside the old ones and only swapped in once they read back correctly.
"""
import ctypes
import ctypes.wintypes as wt
import json
import os
import struct
import sys
import winreg
from pathlib import Path

import echodata as E
import tablet

VERSION = '3.10'
EXE_TIMESTAMP = 1683152886
DATA = Path('_data/5932408047/rad15/win10')
STOCK_PACKAGES = 3
RECORD = E.MID + '.echoqueue.json'
BACKUP = E.MID + '.echoqueue-backup'
OLD_BACKUP = E.MID + '.before-echoqueue'          # left by the first EchoQueue installer
OUR_RESOURCES = {(tablet.CV, tablet.ROOT_CANVAS), (tablet.CV, tablet.NAV_CANVAS), (tablet.CV, tablet.PAGE), (tablet.CV, tablet.OVERLAY),
                 (tablet.BUTTONS, tablet.LEVEL), (tablet.TEX, tablet.ICON_TEXTURE), (tablet.TEX_GPU, tablet.ICON_TEXTURE)}
PLUGIN_FILES = ('EchoQueue.dll', 'EchoQueue.ini', 'EchoQueue.layout')


class InstallError(Exception):
    """Something the user has to sort out; the message says what, in plain words."""


def payload_dir():
    """Where the files that ship inside the installer are (the .exe unpacks them)."""
    return Path(getattr(sys, '_MEIPASS', Path(__file__).resolve().parent)) / 'payload'


# ---- finding Echo ----
def _candidates():
    roots = [Path(r'C:\Program Files\Meta Horizon\Software\Software'), Path(r'C:\Program Files\Oculus\Software\Software'),
             Path(r'C:\echovr'), Path(r'C:\Games')]
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Oculus VR, LLC\Oculus\Libraries') as libs:
            for i in range(64):
                try:
                    guid = winreg.EnumKey(libs, i)
                except OSError:
                    break
                with winreg.OpenKey(libs, guid) as lib:
                    for value in ('OriginalPath', 'Path'):
                        try:
                            path = winreg.QueryValueEx(lib, value)[0]
                        except OSError:
                            continue
                        if path and not path.startswith('\\\\?\\Volume'):
                            roots.append(Path(path) / 'Software')
    except OSError:
        pass
    for root in roots:
        for p in (root / 'ready-at-dawn-echo-arena', root / 'ready-at-dawn-echo-arena' / 'ready-at-dawn-echo-arena', root):
            yield p


def is_game(path):
    path = Path(path)
    return (path / 'bin/win10/echovr.exe').is_file() and (path / DATA / 'manifests' / E.MID).is_file()


def find_game():
    for p in _candidates():
        if is_game(p):
            return p
    return None


def normalise(path):
    """The folder the user picked, or the game folder inside it."""
    path = Path(path)
    for p in (path, path / 'ready-at-dawn-echo-arena', path.parent.parent if path.name == 'win10' else path):
        if is_game(p):
            return p
    return None


def echo_running():
    psapi, kernel = ctypes.WinDLL('psapi'), ctypes.WinDLL('kernel32')
    ids = (wt.DWORD * 4096)()
    got = wt.DWORD()
    if not psapi.EnumProcesses(ids, ctypes.sizeof(ids), ctypes.byref(got)):
        return False
    for pid in ids[:got.value // 4]:
        h = kernel.OpenProcess(0x1000, False, pid)     # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            continue
        try:
            buf = ctypes.create_unicode_buffer(1024)
            size = wt.DWORD(1024)
            if kernel.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)) and buf.value.lower().endswith('\\echovr.exe'):
                return True
        finally:
            kernel.CloseHandle(h)
    return False


def exe_timestamp(exe):
    with open(exe, 'rb') as f:
        head = f.read(4096)
    pe = struct.unpack_from('<I', head, 0x3c)[0]
    return struct.unpack_from('<I', head, pe + 8)[0]


class Game:
    def __init__(self, folder):
        self.folder = Path(folder)
        self.bin = self.folder / 'bin/win10'
        self.plugins = self.bin / 'plugins'
        self.data = self.folder / DATA
        self.manifests = self.data / 'manifests'
        self.packages = self.data / 'packages'
        self.manifest = self.manifests / E.MID
        self.record = self.manifests / RECORD

    def load(self):
        return E.Manifest.load(self.data)

    def read_record(self):
        if not self.record.exists():
            return None
        try:
            return json.loads(self.record.read_text())
        except ValueError:
            raise InstallError(f'{self.record.name} is damaged; EchoQueue cannot tell what it changed.')

    def loader_plugins(self):
        try:
            return [p.get('file', '') for p in json.loads((self.bin / 'echoloader.json').read_text()).get('plugins', [])]
        except (OSError, ValueError, AttributeError):
            return []


def check(game, need_closed=True):
    """Problems that stop install/repair/uninstall: a list of plain-words messages."""
    problems = []
    if not is_game(game.folder):
        return [f'{game.folder} is not an Echo VR folder.']
    if exe_timestamp(game.bin / 'echovr.exe') != EXE_TIMESTAMP:
        problems.append('This Echo VR build is not the one EchoQueue was made for (1683152886); EchoQueue would switch itself off.')
    if not (game.bin / 'dinput8.dll').is_file():
        problems.append('EchoLoader is not installed (bin\\win10\\dinput8.dll is missing). Install EchoLoader first; EchoQueue is one of its plugins.')
    if need_closed and echo_running():
        problems.append('Echo VR is running. Close it first.')
    return problems


# ---- taking EchoQueue out (in memory) ----
def _ours(m, record):
    """-> (package number, [frame indices]) of EchoQueue's data in `m`, or (None, [])."""
    if record and record.get('schema') == 2:
        n = record['package']
        mine = {tuple(f) for f in record['frames']}
        frames = [i for i, f in enumerate(m.frames) if f[0] == n and tuple(f) in mine]
        return (n, frames) if frames else (None, [])
    # First installer (schema 1, or no record): a package holding only EchoQueue's resources.
    for n in range(STOCK_PACKAGES, m.packages):
        frames = [i for i, f in enumerate(m.frames) if f[0] == n]
        users = {(e[0], e[1]) for e in m.entries if m.frames[e[2] & 0xFFFFFFFF][0] == n}
        if frames and (tablet.CV, tablet.PAGE) in users and users <= OUR_RESOURCES:
            return n, frames
    return None, []


def _previous(game, m, record):
    """What EchoQueue's resources pointed at before it: {(type, name): (entry, hash, frame) or None}."""
    if record and record.get('schema') == 2:
        return {(int(p['type'], 16), int(p['name'], 16)): (p['entry'], p['hash'], p['frame']) if p['entry'] else None
                for p in record['previous']}
    backup = game.manifests / OLD_BACKUP
    if not backup.exists():
        raise InstallError('EchoQueue was installed by its first installer, but that installer\'s backup '
                           f'({OLD_BACKUP}) is gone, so EchoQueue cannot be taken out cleanly.')
    old = E.Manifest(backup.read_bytes(), game.data)
    out = {}
    for key in OUR_RESOURCES:
        i = old.find(*key)
        out[key] = None if i is None else (old.entries[i], old.hashes[i], old.frames[old.entries[i][2] & 0xFFFFFFFF])
    return out


def strip(game, m, record):
    """Takes EchoQueue out of manifest `m` in place. -> (package number or None, package now unused)."""
    n, frames = _ours(m, record)
    if n is None:
        return None, False
    first, count = frames[0], len(frames)
    if frames != list(range(first, first + count)):
        raise InstallError("EchoQueue's data is mixed in with another mod's; it can't be taken out safely.")
    previous = _previous(game, m, record)
    keep_slot = n != m.packages - 1                 # another package follows: its number must not move
    markers = [[n, 0, 0, 0]] if keep_slot else []
    shift = count - len(markers)
    by_tuple = {}
    for i, f in enumerate(m.frames):
        if not first <= i < first + count:
            by_tuple.setdefault(tuple(f), i)
    entries, hashes = [], []
    for e, h in zip(m.entries, m.hashes):
        frame = e[2] & 0xFFFFFFFF
        if first <= frame < first + count:
            prev = previous.get((e[0], e[1]))
            if prev is None:
                continue                            # EchoQueue added it: it goes
            pe, ph, pf = prev
            j = by_tuple.get(tuple(pf))
            if j is None:
                raise InstallError('The tablet data EchoQueue replaced has since been removed by another tool, '
                                   'so it cannot be put back.')
            e = [pe[0], pe[1], j | (pe[2] >> 32 << 32), pe[3], pe[4]]
            h = list(ph)
            frame = j
        if frame >= first + count:
            e = [e[0], e[1], (e[2] >> 32 << 32) | (frame - shift), e[3], e[4]]
        entries.append(list(e))
        hashes.append(list(h))
    m.entries, m.hashes = entries, hashes
    m.frames = m.frames[:first] + markers + m.frames[first + count:]
    if not keep_slot:
        m.packages -= 1
        # Empty slots EchoQueue added to step over other tools' stray package files go too, once
        # they're last again.
        slots = set((record or {}).get('slots', []))
        while m.packages - 1 in slots and m.frames and m.frames[-1][0] == m.packages - 1 and m.frames[-1][2] == 0 \
                and sum(f[0] == m.packages - 1 for f in m.frames) == 1:
            m.frames.pop()
            m.packages -= 1
    m._cache = {}
    return n, not keep_slot


def without_echoqueue(game):
    """-> (manifest without EchoQueue, its package number or None, whether that package file is now unused)."""
    m = game.load()
    n, unused = strip(game, m, game.read_record())
    return m, n, unused


# ---- Echo Mods (a mod manager: it switches plugins per profile, and rebuilds the lobby) ----
# Applying an Echo Mods profile switches every plugin to that profile's choice, and plugins a
# profile doesn't name are switched off: EchoQueue has to be named, as on, in its profiles.
# Its lobby rebuilds start from a manifest snapshot (<manifest>.theatre-base) when its marker
# (<manifest>.theatre) is there; a snapshot taken before EchoQueue changed would bring the old
# tablet data back.
ECHOMODS = Path(os.environ.get('ECHOQUEUE_ECHOMODS_DIR') or Path(os.environ.get('APPDATA', '')) / 'EchoMods')  # tests: elsewhere


def echomods_profiles():
    return ECHOMODS / 'profiles.json'


def echomods_present():
    return echomods_profiles().is_file()


def echomods_keep(game, on, log=print):
    """Names EchoQueue in every Echo Mods profile (on), or takes it out again (off)."""
    path = echomods_profiles()
    if not path.is_file():
        return
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
    except ValueError:
        log("Echo Mods' profiles file could not be read; switch EchoQueue on in Echo Mods yourself.")
        return
    changed = False
    for p in data.get('profiles', []):
        plugins = p.setdefault('plugins', {})
        if on and plugins.get('EchoQueue.dll') is not True:
            plugins['EchoQueue.dll'] = True
            changed = True
        elif not on and 'EchoQueue.dll' in plugins:
            del plugins['EchoQueue.dll']
            changed = True
    if changed:
        backup = path.with_name('profiles.json.before-echoqueue')
        if not backup.exists():
            backup.write_bytes(path.read_bytes())
        _replace(path, json.dumps(data, indent=2).encode('utf-8'))
        log('Echo Mods: EchoQueue ' + ('switched on in every profile, so Echo Mods keeps it on.' if on else 'taken out of its profiles.'))
    state_path = game.bin / 'echomods.json'
    if state_path.is_file():
        try:
            state = json.loads(state_path.read_text(encoding='utf-8'))
        except ValueError:
            return
        disabled = state.get('disabled', [])
        kept = [d for d in disabled if str(d.get('file', '')).lower() != 'echoqueue.dll']
        if kept != disabled:
            state['disabled'] = kept
            _replace(state_path, json.dumps(state, indent=4).encode('utf-8'))


def echomods_snapshot_stale(game, record):
    """True when Echo Mods' lobby snapshot would bring back tablet data without the current EchoQueue."""
    marker, snapshot = game.manifests / (E.MID + '.theatre'), game.manifests / (E.MID + '.theatre-base')
    if not (marker.exists() and snapshot.exists() and record and record.get('schema') == 2):
        return False
    try:
        old = E.Manifest(snapshot.read_bytes(), game.data)
    except Exception:
        return True
    mine = {tuple(f) for f in record['frames']}
    return not any(tuple(f) in mine for f in old.frames)


# ---- reporting ----
def scan(game):
    """What's on the tablet now, for the window: dict(state, notes, plugins, problems)."""
    info = dict(problems=check(game, need_closed=False), notes=[], plugins=[], state='not installed')
    if not is_game(game.folder):
        return info
    info['plugins'] = [p for p in game.loader_plugins() if p.lower() != 'echoqueue.dll']
    m = game.load()
    record = None
    try:
        record = game.read_record()
    except InstallError as e:
        info['notes'].append(str(e))
    n, _ = _ours(m, record)
    dll = (game.plugins / 'EchoQueue.dll').exists()
    if n is not None:
        info['state'] = f"installed (v{record.get('version', '?')})" if record and record.get('schema') == 2 else 'installed (first installer)'
        if not dll or not (game.plugins / 'EchoQueue.layout').exists():
            info['state'] += ' - plugin files missing, use Repair'
    elif record or dll:
        info['state'] = 'PLAY tab missing (another tool rewrote the tablet data) - use Repair'
    try:
        base, _, _ = without_echoqueue(game)
        built = tablet.build(base)
        info['notes'] += built['notes']
        info['tabs'] = built['tabs']
        info['preview'] = tablet.tab_preview(base, built)
    except (tablet.TabletError, InstallError) as e:
        info['problems'].append(str(e))
    info['echomods'] = echomods_present()
    if info['echomods'] and n is not None and echomods_snapshot_stale(game, record):
        info['notes'].append("Echo Mods' saved lobby snapshot is from before this EchoQueue: if Echo Mods rebuilds "
                             'the lobby, run Repair here afterwards.')
    return info


# ---- writing ----
def _replace(path, data):
    tmp = path.with_name(path.name + '.echoqueue-new')
    tmp.write_bytes(data)
    os.replace(tmp, path)


def _set_loader(game, present):
    path = game.bin / 'echoloader.json'
    try:
        config = json.loads(path.read_text()) if path.exists() else {'plugins': []}
    except ValueError:
        raise InstallError('bin\\win10\\echoloader.json is damaged; fix or delete it, then try again.')
    plugins = [p for p in config.setdefault('plugins', []) if str(p.get('file', '')).lower() != 'echoqueue.dll']
    if present:
        plugins.append({'file': 'EchoQueue.dll', 'args': {}})
    config['plugins'] = plugins
    _replace(path, json.dumps(config, indent=4).encode())


def _write_game_data(game, m, new_package=None):
    """Backs the manifest up, then writes `m` (and our package if given) and checks it reads back."""
    _replace(game.manifests / BACKUP, game.manifest.read_bytes())
    if new_package:
        number, blob = new_package
        _replace(game.packages / f'{E.MID}_{number}', blob)
    _replace(game.manifest, m.serialize())
    E.Manifest.load(game.data)        # reads back


def _no_stage(name):
    pass


def install(game, log=print, stage=_no_stage):
    """Install, or repair: EchoQueue out, then the tab rebuilt on the tablet as it is now.
    stage(name) is called as it goes: check, build, write, verify, plugin (for a progress bar)."""
    stage('check')
    problems = check(game)
    if problems:
        raise InstallError('\n'.join(problems))
    payload = payload_dir()
    if not (payload / 'EchoQueue.dll').is_file():
        raise InstallError('The installer is missing EchoQueue.dll.')
    m, old, unused = without_echoqueue(game)
    if old is not None:
        log(f'Took the existing EchoQueue out of the tablet data (package _{old}).')
    stage('build')
    try:
        built = tablet.build(m)
    except tablet.TabletError as e:
        raise InstallError(str(e))
    for note in built['details']:
        log(note)
    previous = []
    for t, n, _ in built['resources']:
        i = m.find(t, n)
        previous.append(dict(type=f'{t:016x}', name=f'{n:016x}',
                             entry=m.entries[i] if i is not None else None,
                             hash=m.hashes[i] if i is not None else None,
                             frame=m.frames[m.entries[i][2] & 0xFFFFFFFF] if i is not None else None))
    # Package files past the manifest's last one belong to other tools: keep their numbers.
    slots = []
    while (game.packages / f'{E.MID}_{m.packages}').exists() and m.packages != old:
        size = (game.packages / f'{E.MID}_{m.packages}').stat().st_size
        log(f'Package _{m.packages} is not in the game data (left by another tool); leaving it alone.')
        m.frames.append([m.packages, size, 0, 0])
        slots.append(m.packages)
        m.packages += 1
    stage('write')
    number, blob = m.add_package(built['resources'])
    frames = [f for f in m.frames if f[0] == number]
    _write_game_data(game, m, (number, blob))
    if old is not None and old != number and unused:
        stale = game.packages / f'{E.MID}_{old}'
        if stale.exists():
            stale.unlink()
    stage('verify')
    check_m = game.load()
    for t, n, data in built['resources']:
        if check_m.get(t, n) != data:
            raise InstallError('The tablet data did not read back correctly after writing. '
                               f'The previous manifest is in manifests\\{BACKUP}.')
    record = dict(schema=2, version=VERSION, package=number, frames=frames, previous=previous, slots=slots,
                  manifest_sha256=E.sha256(game.manifest.read_bytes()))
    _replace(game.record, json.dumps(record, indent=1).encode())
    stage('plugin')
    game.plugins.mkdir(exist_ok=True)
    _replace(game.plugins / 'EchoQueue.dll', (payload / 'EchoQueue.dll').read_bytes())
    if not (game.plugins / 'EchoQueue.ini').exists():
        _replace(game.plugins / 'EchoQueue.ini', (payload / 'EchoQueue.ini').read_bytes())
    _replace(game.plugins / 'EchoQueue.layout', tablet.layout_ini(built['layout']).encode())
    _set_loader(game, True)
    echomods_keep(game, True, log)
    log(f"Installed EchoQueue v{VERSION}: PLAY tab added to package _{number}, {built['tabs']} tabs on the bar.")
    return built


def launch(game):
    """Start Echo VR (bin\\win10\\echovr.exe, from its own folder), as its shortcut would."""
    import subprocess
    if echo_running():
        raise InstallError('Echo VR is already running.')
    exe = game.bin / 'echovr.exe'
    if not exe.is_file():
        raise InstallError("Echo VR isn't in this folder.")
    subprocess.Popen([str(exe)], cwd=str(game.bin), close_fds=True,
                     creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP)


def uninstall(game, log=print, stage=_no_stage):
    stage('check')
    problems = check(game)
    if problems and not any('not the one EchoQueue' in p or 'EchoLoader' in p for p in problems):
        raise InstallError('\n'.join(problems))
    if echo_running():
        raise InstallError('Echo VR is running. Close it first.')
    m, n, unused = without_echoqueue(game)
    stage('write')
    if n is not None:
        _write_game_data(game, m)
        if unused:
            p = game.packages / f'{E.MID}_{n}'
            if p.exists():
                p.unlink()
        log(f"Took the PLAY tab out of the tablet data (package _{n}); other mods' changes kept.")
    else:
        log('The PLAY tab was not in the tablet data.')
    for name in (RECORD, OLD_BACKUP):
        p = game.manifests / name
        if p.exists():
            p.unlink()
    stage('plugin')
    for name in ('EchoQueue.dll', 'EchoQueue.layout'):
        p = game.plugins / name
        if p.exists():
            p.unlink()
    _set_loader(game, False)
    echomods_keep(game, False, log)
    log('Removed the EchoQueue plugin. Your settings (plugins\\EchoQueue.ini) and log are kept.')
