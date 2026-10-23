"""Install or remove the PLAY tab data built by build_queue_tab.py (Echo must be closed).

    python install_tab.py install --game "<...>\\ready-at-dawn-echo-arena"
    python install_tab.py restore --game "<...>\\ready-at-dawn-echo-arena"

install: only onto the exact manifest the tab was built from; backs it up first as
         manifests/48037dc70b0ecab2.before-echoqueue, then copies our package and manifest.
restore: takes EchoQueue back out of the manifest the game has now, leaving every other
         mod's changes in place: each resource that points into our package gets the entry
         it had before EchoQueue (from the before-echoqueue backup) or, if EchoQueue added
         it, is removed; our frames go, and our package file too when it's the last one (a
         package another mod added after ours keeps its number, so ours stays as an unused
         slot). The manifest as it was is saved first as 48037dc70b0ecab2.before-restore.
         --from-backup puts the before-echoqueue backup back whole instead (drops other
         mods' later changes).
"""
import argparse
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path

MID = '48037dc70b0ecab2'
BUILD = Path(__file__).resolve().parent.parent / 'build/tab'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def strip(manifest, backup, packages, our_names):
    """Remove EchoQueue's package(s) from `manifest` in place; returns False if none found."""
    from echovr_patch import ManifestFile
    current, before = ManifestFile(manifest.read_bytes()), ManifestFile(backup.read_bytes())
    ours = {int(n.rsplit('_', 1)[1]) for n in our_names}
    frames = [i for i, c in enumerate(current.C) if c[0] in ours]
    if not frames:
        return False
    if frames != list(range(frames[0], frames[-1] + 1)):
        sys.exit('EchoQueue\'s frames are not in one block; not touching the manifest.')
    first, count = frames[0], len(frames)
    last = len(ours) == 1 and max(ours) == current.npkg - 1
    slots = [] if last else sorted(ours)  # end markers kept for package numbers that must stay
    shift = count - len(slots)
    restored = removed = 0
    keep = []
    for a, b in zip(current.A, current.B):
        frame = a[2] & 0xffffffff
        if first <= frame < first + count:
            prev = before.index.get((a[0], a[1]))
            if prev is None:
                removed += 1
                continue
            pa, pb = list(before.A[prev]), list(before.B[prev])
            if (pa[2] & 0xffffffff) >= first:
                sys.exit(f'The pre-EchoQueue entry for {a[0]:016x}:{a[1]:016x} points past the stock frames; not touching the manifest.')
            a, b = pa, pb
            restored += 1
        elif frame >= first + count:  # frames another mod appended after ours move down
            a = list(a)
            a[2] = (a[2] & ~0xffffffff) | (frame - shift)
        keep.append((a, b))
    current.A = [list(a) for a, _ in keep]
    current.B = [list(b) for _, b in keep]
    # Another mod's package comes after ours: keep our number as an empty slot (end marker only).
    markers = [[n, (packages / f'{MID}_{n}').stat().st_size if (packages / f'{MID}_{n}').exists() else 0, 0, 0] for n in slots]
    current.C = current.C[:first] + markers + current.C[first + count:]
    if last:
        current.npkg -= 1
    current.index = {(a[0], a[1]): i for i, a in enumerate(current.A)}
    manifest.write_bytes(current.serialize())
    check = ManifestFile(manifest.read_bytes())
    if len(check.A) != len(current.A) or check.npkg != current.npkg:
        sys.exit('The rewritten manifest does not read back; restore 48037dc70b0ecab2.before-restore by hand.')
    print(f'took EchoQueue out of {manifest.name}: {restored} entries back to their pre-EchoQueue state, '
          f'{removed} removed, {count} frames dropped; other mods\' entries kept')
    if last:
        for n in ours:
            p = packages / f'{MID}_{n}'
            if p.exists():
                p.unlink()
                print('removed', p.name)
    else:
        print('another mod\'s package comes after ours, so our package number stays as an empty slot')
    return True


def main():
    a = argparse.ArgumentParser()
    a.add_argument('command', choices=('install', 'restore', 'status'))
    a.add_argument('--game', required=True)
    a.add_argument('--tools', default=str(Path(os.environ.get('TEMP', '.')) / 'echoq/tt'),
                   help="Doom-on-EchoVR's EchoVr-Tablet-Probing folder (manifest reader), for restore")
    a.add_argument('--from-backup', action='store_true', help='restore: put the whole pre-EchoQueue manifest back')
    a = a.parse_args()
    data = Path(a.game) / '_data/5932408047/rad15/win10'
    manifest, packages = data / 'manifests' / MID, data / 'packages'
    backup = data / 'manifests' / (MID + '.before-echoqueue')
    record = data / 'manifests' / (MID + '.echoqueue.json')
    meta = json.loads((BUILD / 'queue_tab.json').read_text()) if (BUILD / 'queue_tab.json').exists() else {}
    installed = json.loads(record.read_text())['packages'] if record.exists() else []
    current = sha(manifest)
    if a.command == 'status':
        print('manifest:', 'PLAY tab installed' if current == meta.get('manifest_sha256') else
              'as built from (tab not installed)' if current == meta.get('base_manifest_sha256') else 'changed by something else')
        print('backup:', backup.exists(), ' package:', bool(meta) and (packages / meta['package']).exists())
        return 0
    if a.command == 'install':
        ours = packages / meta['package']
        if current == meta['manifest_sha256']:
            print('Already installed.')
            return 0
        if current != meta['base_manifest_sha256']:
            sys.exit('The game manifest changed since the tab was built (another mod?). Rebuild with build_queue_tab.py first.')
        if ours.exists():
            sys.exit(f'{ours.name} already exists; not overwriting it.')
        shutil.copy2(manifest, backup)
        if sha(backup) != current:
            sys.exit('Backup copy does not match; nothing installed.')
        print('backed up', manifest, '->', backup.name)
        record.write_text(json.dumps({'packages': sorted(set(installed) | {ours.name})}))
        shutil.copy2(BUILD / 'packages' / meta['package'], ours)
        shutil.copy2(BUILD / 'manifests' / MID, manifest)
        ok = sha(ours) == meta['package_sha256'] and sha(manifest) == meta['manifest_sha256']
        print('installed', ours.name, 'and the manifest:', 'verified' if ok else 'VERIFY FAILED')
        return 0 if ok else 1
    if not backup.exists():
        sys.exit('No backup found; nothing to restore.')
    if a.from_backup:
        shutil.copy2(backup, manifest)
        print('restored', manifest.name, 'from', backup.name, '(other mods\' later changes dropped)')
        for name in sorted(installed):
            p = packages / name
            if p.exists():
                p.unlink()
                print('removed', name)
    else:
        if not installed:
            print('EchoQueue is not recorded as installed; nothing to take out.')
            return 0
        os.environ.setdefault('ECHOVR_GAME', a.game)
        sys.path[:0] = [a.tools, str(Path(a.tools).parent / 'lib')]  # the tools, and their pip packages (zstandard)
        shutil.copy2(manifest, data / 'manifests' / (MID + '.before-restore'))
        if not strip(manifest, backup, packages, installed):
            print('No EchoQueue frames in the manifest (already taken out?).')
    if record.exists():
        record.unlink()
    return 0


if __name__ == '__main__':
    sys.exit(main())
