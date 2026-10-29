"""Builds dist/EchoLazyTabletInstaller.exe (run build.cmd first, for dist/EchoLazyTablet.dll).

    python installer/build.py

The installer bundles: EchoLazyTablet.dll and the default EchoLazyTablet.ini (payload/), this folder's
Python code, Pillow, python-zstandard, texture2ddecoder and pywebview (its own window, on
Windows' WebView2) with pythonnet (pydeps/), and Python itself.
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


# The window: pywebview and what it runs on.
WINDOW_LICENCES = [('pywebview', DEPS / 'pywebview-5.4.dist-info/LICENSE'),
                   ('pythonnet', DEPS / 'pythonnet-3.2.1.dist-info/licenses/LICENSE'),
                   ('clr_loader', DEPS / 'clr_loader-0.3.1.dist-info/licenses/LICENSE'),
                   ('bottle', DEPS / 'bottle-0.13.4.dist-info/licenses/LICENSE'),
                   ('cffi', DEPS / 'cffi-2.1.1.dist-info/licenses/LICENSE'),
                   ('pycparser', DEPS / 'pycparser-3.12.dist-info/licenses/LICENSE'),
                   ('typing_extensions', DEPS / 'typing_extensions-4.16.0.dist-info/licenses/LICENSE')]
WINDOW_NOTES = [('proxy_tools', 'proxy_tools 0.1.0 (used by pywebview): MIT licence, as its package metadata states; '
                                'it ships no licence file.'),
                ('Microsoft WebView2 SDK (in pywebview)', 'Microsoft.Web.WebView2.Core.dll and Microsoft.Web.WebView2.WinForms.dll: '
                 'Copyright (C) Microsoft Corporation, under the WebView2 SDK licence '
                 '(https://www.nuget.org/packages/Microsoft.Web.WebView2/, "License" link). The WebView2 '
                 'Runtime itself is part of Windows and is not included.')]


def notices(first, sources, notes=()):
    parts = [first, *notes]
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


def version_info(path):
    """The exe's Windows version resource (Properties > Details), from patcher.VERSION."""
    sys.path.insert(0, str(HERE))
    from patcher import VERSION
    nums = (tuple(int(n) for n in VERSION.split('.')) + (0, 0, 0, 0))[:4]
    fields = {'CompanyName': 'EchoLazyTablet', 'FileDescription': 'EchoLazyTablet installer',
              'FileVersion': VERSION, 'InternalName': 'EchoLazyTabletInstaller',
              'LegalCopyright': 'EchoLazyTablet author', 'OriginalFilename': 'EchoLazyTabletInstaller.exe',
              'ProductName': 'EchoLazyTablet', 'ProductVersion': VERSION}
    strings = ',\n'.join(f'          StringStruct({k!r}, {v!r})' for k, v in fields.items())
    path.write_text(f"""VSVersionInfo(
  ffi=FixedFileInfo(filevers={nums}, prodvers={nums}, mask=0x3f, flags=0x0, OS=0x40004,
                    fileType=0x1, subtype=0x0, date=(0, 0)),
  kids=[
    StringFileInfo([StringTable('040904B0', [
{strings}])]),
    VarFileInfo([VarStruct('Translation', [1033, 1200])])
  ]
)
""", encoding='utf-8')


def main():
    dll = ROOT / 'dist/EchoLazyTablet.dll'
    if not dll.exists():
        raise SystemExit('Build the plugin first (build.cmd).')
    if PAYLOAD.exists():
        shutil.rmtree(PAYLOAD)
    PAYLOAD.mkdir()
    shutil.copy2(dll, PAYLOAD / 'EchoLazyTablet.dll')
    shutil.copy2(ROOT / 'EchoLazyTablet.ini', PAYLOAD / 'EchoLazyTablet.ini')
    text = notices(('EchoLazyTablet', 'EchoLazyTablet and its installer: by the EchoLazyTablet author. The tablet art is drawn at install time '
                                 "from the player's own copy of Echo VR; no game files are included."),
                   [('MinHook (in EchoLazyTablet.dll)', ROOT / 'vendor/minhook/LICENSE.txt'),
                    ('Pillow', DEPS / 'pillow-11.3.0.dist-info/licenses/LICENSE'),
                    ('python-zstandard (includes Zstandard)', DEPS / 'zstandard-0.25.0.dist-info/licenses/LICENSE'),
                    ('texture2ddecoder', DEPS / 'texture2ddecoder-1.0.5.dist-info/LICENSE'),
                    *WINDOW_LICENCES,
                    ('Python', Path(sys.base_prefix) / 'LICENSE.txt'),
                    ('Tcl/Tk (one licence for both)', Path(sys.base_prefix) / 'tcl/tk8.6/license.terms')],
                   WINDOW_NOTES)
    (PAYLOAD / 'THIRD_PARTY_NOTICES.txt').write_text(text, encoding='utf-8')
    work = ROOT / 'build/installer'
    work.mkdir(parents=True, exist_ok=True)
    icon(work / 'echoqueue.ico')
    version_info(work / 'version.txt')
    # A folder, not --onefile: a one-file exe unpacks itself to %TEMP% and runs from there, which
    # Windows Defender's machine-learning check flags (Program:Win32/Contebrew.A!ml).
    subprocess.check_call([sys.executable, '-m', 'PyInstaller', '--noconfirm', '--clean', '--onedir', '--windowed',
                           '--noupx', '--version-file', str(work / 'version.txt'),
                           '--name', 'EchoLazyTabletInstaller', '--icon', str(work / 'echoqueue.ico'),
                           '--paths', str(DEPS), '--paths', str(HERE),
                           '--hidden-import', 'zstandard.backend_c', '--hidden-import', 'texture2ddecoder',
                           '--add-data', f'{PAYLOAD};payload', '--add-data', f'{HERE / "web"};web',
                           '--distpath', str(ROOT / 'dist'), '--workpath', str(work), '--specpath', str(work),
                           '--hidden-import', 'webapp', '--hidden-import', 'webview', str(HERE / 'app.py')])
    folder = ROOT / 'dist/EchoLazyTabletInstaller'
    (folder / 'THIRD_PARTY_NOTICES.txt').write_text(text, encoding='utf-8')
    shutil.copy2(ROOT / 'README.txt', folder / 'README.txt')
    (ROOT / 'dist/THIRD_PARTY_NOTICES.txt').write_text(text, encoding='utf-8')
    shutil.copy2(ROOT / 'README.txt', ROOT / 'dist/README.txt')
    archive = shutil.make_archive(str(ROOT / 'dist/EchoLazyTabletInstaller'), 'zip', ROOT / 'dist', folder.name)
    print('built', folder / 'EchoLazyTabletInstaller.exe', 'and', archive)


if __name__ == '__main__':
    main()
