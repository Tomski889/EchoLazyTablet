"""The player's own Echo VR data, for the tablet mock-up in the installer's window. Read-only.

  * %LOCALAPPDATA%\\RAD\\EchoVR\\users\\<platform>\\<id>\\serverprofile.json (the newest one, as the
    plugin's MATCHES OVERALL uses): display name, arena level, games played, win percentage.
  * %LOCALAPPDATA%\\rad\\loneecho\\settings_mp_v2.json: the HOME page's Personal Bubble, Mute All
    and Ghost All switches.
"""
import json
import os
import re
from pathlib import Path


def _local():
    return Path(os.environ.get('LOCALAPPDATA', ''))


def _newest_profile():
    users = _local() / 'RAD' / 'EchoVR' / 'users'
    try:
        files = list(users.glob('*/*/serverprofile.json'))
    except OSError:
        return None
    return max(files, key=lambda f: f.stat().st_mtime, default=None)


def _read(path):
    try:
        return json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError, AttributeError):
        return None


def _stat(stats, mode, name):
    v = stats.get(mode, {}).get(name, {})
    return v.get('val') if isinstance(v, dict) else None


def info():
    """dict(name, level, matches, win, bubble, mute, ghost); values None when not found."""
    out = dict(name=None, level=None, matches=None, win=None, bubble=None, mute=None, ghost=None)
    profile = _read(_newest_profile())
    if isinstance(profile, dict):
        stats = profile.get('stats') or {}
        out['name'] = profile.get('displayname')
        out['level'] = _stat(stats, 'arena', 'Level')
        played = [_stat(stats, m, 'GamesPlayed') for m in ('arena', 'combat')]
        if any(p is not None for p in played):
            out['matches'] = int(sum(p or 0 for p in played))
        out['win'] = _stat(stats, 'arena', 'ArenaWinPercentage')
    settings = _read(_local() / 'rad' / 'loneecho' / 'settings_mp_v2.json')
    if isinstance(settings, dict):
        game = settings.get('game') or {}
        out['bubble'] = game.get('EnablePersonalBubble')
        out['mute'] = game.get('EnableMuteAll')
        out['ghost'] = game.get('EnableGhostAll')
    return out


# ---- who's in your session, from the game's own logs ----
# <game>\_local\r14logs\*.log lists every entrant as they arrive ("Name : x", then "User Slot : n",
# "Is Bot : ...") and "Removing Entrant (slot: n)" / "Removing all users..." as they go.
_LINE = re.compile(r'\]: \[NETGAME\] (?:\t(Name|User Slot|Is Bot|Is Spectator)\s*: (.*)|Removing Entrant \(slot: (\d+)\)'
                   r'|(Removing all users)|NetGame switching state \(from [^,]*, to ([^)]*)\))')
_cache = {}


def _roster(path):
    """(people there at the end of the log, the last time anyone was, and where: 'lobby' or 'in game')."""
    slots, block, last, where, last_where = {}, None, [], '', ''
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if '[NETGAME]' not in line:
                continue
            m = _LINE.search(line)
            if not m:
                continue
            field, value, removed, cleared, state = m.groups()
            if field == 'Name':
                block = dict(name=value.strip(), bot=False, spectator=False)
            elif field == 'User Slot' and block:
                slots[value.strip()] = block
            elif field in ('Is Bot', 'Is Spectator') and block:
                block['bot' if field == 'Is Bot' else 'spectator'] = value.strip() == 'true'
            elif removed is not None:
                slots.pop(removed, None)
            elif cleared:
                slots = {}
            elif state:
                if state in ('lobby', 'in game'):   # (not the states on the way out)
                    where = state
                continue
            people = [p for p in slots.values() if not p['bot']]
            if people:
                last, last_where = people, where
    now = [p for p in slots.values() if not p['bot']]
    return now, last, (where if now else last_where)


def _logs(game):
    try:
        folder = Path(game) / '_local' / 'r14logs'
        return sorted(folder.glob('*.log'), key=lambda f: f.stat().st_mtime, reverse=True)
    except OSError:
        return []


def _cached(path):
    key = (str(path), path.stat().st_mtime, path.stat().st_size)
    if key not in _cache:
        if len(_cache) > 64:
            _cache.clear()
        _cache[key] = _roster(path)
    return _cache[key]


def people(game, me=None, running=False, recent_logs=12):
    """dict(lobby=[{name, spectator}], live, where, at, recent=[name]) from the newest logs."""
    out = dict(lobby=[], live=False, where='', at=None, recent=[])
    logs = _logs(game) if game else []
    seen = []
    for i, path in enumerate(logs[:recent_logs]):
        try:
            now, last, where = _cached(path)
        except OSError:
            continue
        if i == 0 and running and now:          # Echo is open: who's there right now
            out.update(lobby=now, live=True, where=where, at=path.stat().st_mtime)
        elif not out['lobby'] and last:         # otherwise the last session it logged
            out.update(lobby=last, where=where, at=path.stat().st_mtime)
        for p in last + now:
            if p['name'] != me and p['name'] not in seen:
                seen.append(p['name'])
    out['recent'] = seen[:8]
    me_first = sorted(out['lobby'], key=lambda p: p['name'] != me)
    out['lobby'] = [dict(name=p['name'], spectator=p['spectator']) for p in me_first]
    return out
