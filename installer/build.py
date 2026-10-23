"""Builds dist/EchoQueueInstaller.exe (run build.cmd first, for dist/EchoQueue.dll).

    python installer/build.py

The installer bundles: EchoQueue.dll and the default EchoQueue.ini (payload/), this folder's
Python code, Pillow, python-zstandard and texture2ddecoder (pydeps/), and Python itself.
THIRD_PARTY_NOTICES.txt goes inside it and next to it.
"""
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PAYLOAD = HERE / 'payload'
DEPS = HERE / 'pydeps'


def notices():
    parts = [('EchoQueue', 'EchoQueue and its installer: by the EchoQueue author. The tablet art is drawn at install time '
                           "from the player's own copy of Echo VR; no game files are included.")]
    sources = [('MinHook (in EchoQueue.dll)', ROOT / 'vendor/minhook/LICENSE.txt'),
               ('Pillow', DEPS / 'pillow-11.3.0.dist-info/licenses/LICENSE'),
               ('python-zstandard (includes Zstandard)', DEPS / 'zstandard-0.25.0.dist-info/licenses/LICENSE'),
               ('texture2ddecoder', DEPS / 'texture2ddecoder-1.0.5.dist-info/LICENSE'),
               ('Python', Path(sys.base_prefix) / 'LICENSE.txt'),
               ('Tcl/Tk (one licence for both)', Path(sys.base_prefix) / 'tcl/tk8.6/license.terms')]
    for name, path in sources:
        if not path.exists():
            raise SystemExit(f'Licence for {name} not found at {path}')
        parts.append((name, path.read_text(encoding='utf-8', errors='replace')))
    return '\n\n'.join(f'==== {name} ====\n\n{text.strip()}' for name, text in parts) + '\n'


def icon(path):
    sys.path.insert(0, str(DEPS))
    sys.path.insert(0, str(HERE))
    from PIL import Image
    from disc_icon import disc
    base = Image.new('RGBA', (256, 256), (13, 117, 115, 255))
    g = disc(208)
    base.alpha_composite(g, (24, 24))
    base.save(path, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])


def main():
    dll = ROOT / 'dist/EchoQueue.dll'
    if not dll.exists():
        raise SystemExit('Build the plugin first (build.cmd).')
    if PAYLOAD.exists():
        shutil.rmtree(PAYLOAD)
    PAYLOAD.mkdir()
    shutil.copy2(dll, PAYLOAD / 'EchoQueue.dll')
    shutil.copy2(ROOT / 'EchoQueue.ini', PAYLOAD / 'EchoQueue.ini')
    text = notices()
    (PAYLOAD / 'THIRD_PARTY_NOTICES.txt').write_text(text, encoding='utf-8')
    work = ROOT / 'build/installer'
    work.mkdir(parents=True, exist_ok=True)
    icon(work / 'echoqueue.ico')
    subprocess.check_call([sys.executable, '-m', 'PyInstaller', '--noconfirm', '--clean', '--onefile', '--windowed',
                           '--name', 'EchoQueueInstaller', '--icon', str(work / 'echoqueue.ico'),
                           '--paths', str(DEPS), '--paths', str(HERE),
                           '--hidden-import', 'zstandard.backend_c', '--hidden-import', 'texture2ddecoder',
                           '--add-data', f'{PAYLOAD};payload', '--add-data', f'{HERE / "web"};web',
                           '--distpath', str(ROOT / 'dist'), '--workpath', str(work), '--specpath', str(work),
                           '--hidden-import', 'webapp', str(HERE / 'app.py')])
    (ROOT / 'dist/THIRD_PARTY_NOTICES.txt').write_text(text, encoding='utf-8')
    shutil.copy2(ROOT / 'README.txt', ROOT / 'dist/README.txt')
    print('built', ROOT / 'dist/EchoQueueInstaller.exe')


if __name__ == '__main__':
    main()
