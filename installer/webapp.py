"""The installer's window: a small local web server (127.0.0.1, random port) serving web/, shown in
the installer's own window (pywebview on Windows' WebView2: no browser, no address bar); the
installer exits when the window closes. Without WebView2 it falls back to an Edge app window. Every API call must carry the random token from the window's URL,
so no other page in a browser can drive it."""
import ctypes
import json
import os
import secrets
import subprocess
import sys
import threading
import time
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

import patcher
import player

TOKEN = secrets.token_urlsafe(24)
BYE_AT = None             # when the page last said goodbye (closing, or reloading)
LAST_SEEN = None          # when the window last called (it polls every 0.6 s)
ISSUES = 'https://github.com/Tomski889/EchoLazyTablet/issues/new'   # where Report a bug goes
WEB = Path(getattr(sys, '_MEIPASS', Path(__file__).resolve().parent)) / 'web'
TYPES = {'.html': 'text/html; charset=utf-8', '.css': 'text/css', '.js': 'application/javascript', '.svg': 'image/svg+xml'}


class State:
    def __init__(self):
        self.lock = threading.Lock()
        found = os.environ.get('ECHOQUEUE_GAME') or patcher.find_game()      # tests: a copy of the game
        self.game = str(found) if found else ''
        self.scan = None            # scan result for self.game
        self.scanning = False
        self.job = dict(busy=False, kind='', log=[], stage='', ok=None, message='', admin=False)
        self.player, self.player_at = None, -1e9

    def view(self):
        now = time.monotonic()
        if now - self.player_at > 3:            # the tablet mock-up's pages: re-read every few seconds
            info = player.info()
            try:
                info['people'] = player.people(self.game, me=info['name'], running=patcher.echo_running())
            except Exception:
                info['people'] = None
            self.player, self.player_at = info, now
        with self.lock:
            return dict(version=patcher.VERSION, game=self.game, scan=self.scan, scanning=self.scanning, job=dict(self.job),
                        player=self.player)

    def rescan(self):
        with self.lock:
            if self.scanning or self.job['busy']:
                return
            self.scanning = True
            folder = self.game

        def work():
            path = patcher.normalise(folder) if folder else None
            if not path:
                result = dict(found=False)
            else:
                try:
                    result = dict(found=True, **patcher.scan(patcher.Game(path)))
                except Exception as e:
                    result = dict(found=True, state='unknown', problems=[f'Could not read the game data: {e}'], notes=[], plugins=[])
            with self.lock:
                if path:
                    self.game = str(path)
                self.scan, self.scanning = result, False
        threading.Thread(target=work, daemon=True).start()

    def run(self, kind):
        with self.lock:
            if self.job['busy'] or not self.game:
                return False
            self.job = dict(busy=True, kind=kind, log=[], stage='check', ok=None, message='', admin=False)
            game = patcher.Game(Path(self.game))

        def log(line):
            with self.lock:
                self.job['log'].append(line)

        def stage(name):
            with self.lock:
                self.job['stage'] = name

        def work():
            ok, message, admin = False, '', False
            try:
                (patcher.install if kind == 'install' else patcher.uninstall)(game, log=log, stage=stage)
                ok = True
                message = ('Start Echo VR: PLAY is the first tab on your hand tablet.' if kind == 'install'
                           else 'EchoLazyTablet has been removed. Your other mods are as they were.')
            except patcher.InstallError as e:
                message = str(e)
            except PermissionError as e:
                message = f'Windows would not let the installer write to the game folder ({e.filename}).'
                admin = True
            except Exception:
                message = 'Something went wrong:\n' + traceback.format_exc(limit=3)
            with self.lock:
                self.job.update(busy=False, ok=ok, message=message, admin=admin)
            self.rescan()
        threading.Thread(target=work, daemon=True).start()
        return True


STATE = State()


WINDOW = None             # the installer's own window (pywebview), once open


def browse():
    """The Windows folder picker; -> chosen folder or ''."""
    if WINDOW is not None:
        import webview
        picked = WINDOW.create_file_dialog(webview.FOLDER_DIALOG)
        return picked[0] if picked else ''
    import tkinter
    from tkinter import filedialog
    root = tkinter.Tk()
    root.withdraw()
    root.attributes('-topmost', True)
    try:
        return filedialog.askdirectory(parent=root, title='Pick the ready-at-dawn-echo-arena folder') or ''
    finally:
        root.destroy()


def relaunch_as_admin():
    exe = sys.executable
    params = '' if getattr(sys, 'frozen', False) else f'"{Path(__file__).resolve().parent / "app.py"}"'
    ctypes.windll.shell32.ShellExecuteW(None, 'runas', exe, params, None, 1)
    os._exit(0)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send(self, code, body, kind='application/json'):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header('Content-Type', kind)
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = urlparse(self.path).path
        if path.startswith('/api/'):
            return self.api(path, {})
        name = 'index.html' if path in ('/', '') else path.lstrip('/')
        file = (WEB / name).resolve()
        if WEB.resolve() not in file.parents or not file.is_file():
            return self.send(404, b'not found', 'text/plain')
        self.send(200, file.read_bytes(), TYPES.get(file.suffix, 'application/octet-stream'))

    def do_POST(self):
        if urlparse(self.path).path == '/api/bye' and urlparse(self.path).query == 't=' + TOKEN:
            global BYE_AT
            BYE_AT = time.monotonic()   # a beacon from the page as it goes (it can't send headers)
            return self.send(200, {})
        length = int(self.headers.get('Content-Length') or 0)
        try:
            body = json.loads(self.rfile.read(length) or b'{}')
        except ValueError:
            body = {}
        self.api(urlparse(self.path).path, body)

    def api(self, path, body):
        if self.headers.get('X-Token') != TOKEN:
            return self.send(403, {'error': 'forbidden'})
        global LAST_SEEN
        LAST_SEEN = time.monotonic()
        if path == '/api/state':
            return self.send(200, STATE.view())
        if path == '/api/rescan':
            STATE.rescan()
            return self.send(200, STATE.view())
        if path == '/api/folder':
            with STATE.lock:
                STATE.game = str(body.get('path') or '')
                STATE.scan = None
            STATE.rescan()
            return self.send(200, STATE.view())
        if path == '/api/browse':
            picked = browse()
            if picked:
                with STATE.lock:
                    STATE.game, STATE.scan = picked, None
                STATE.rescan()
            return self.send(200, STATE.view())
        if path in ('/api/install', '/api/uninstall'):
            STATE.run(path.rsplit('/', 1)[1])
            return self.send(200, STATE.view())
        if path == '/api/launch':
            try:
                patcher.launch(patcher.Game(STATE.game))
                return self.send(200, {'ok': True})
            except patcher.InstallError as e:
                return self.send(200, {'ok': False, 'message': str(e)})
            except OSError as e:
                return self.send(200, {'ok': False, 'message': f"Couldn't start Echo VR: {e.strerror or e}"})
        if path == '/api/bug':
            import webbrowser
            from urllib.parse import urlencode
            body = (f'**EchoLazyTablet version:** {patcher.VERSION}\n\n**What happened:**\n\n\n'
                    '**What you expected:**\n\n\n**Steps to make it happen again:**\n1. \n')
            webbrowser.open(ISSUES + '?' + urlencode(dict(title='Bug: ', body=body, labels='bug')))
            return self.send(200, {})
        if path == '/api/admin':
            threading.Timer(.3, relaunch_as_admin).start()
            return self.send(200, {})
        self.send(404, {'error': 'unknown'})


def edge():
    for base in (os.environ.get('ProgramFiles(x86)', ''), os.environ.get('ProgramFiles', ''), os.environ.get('LOCALAPPDATA', '')):
        p = os.path.join(base, 'Microsoft', 'Edge', 'Application', 'msedge.exe')
        if os.path.isfile(p):
            return p
    return None


def own_window(url):
    """Shows the page in the installer's own window until it's closed. -> False if it can't
    (no WebView2 on this PC), for the Edge fallback."""
    global WINDOW
    try:
        import webview
    except Exception:
        return False
    try:
        WINDOW = webview.create_window('EchoLazyTablet', url, width=1000, height=780, min_size=(760, 600),
                                       background_color='#070a10')
        webview.start(gui='edgechromium', private_mode=True)
        return True
    except Exception:
        WINDOW = None
        return False
    finally:
        if STATE.job['busy']:     # closed mid-install: let it finish before exiting
            while STATE.job['busy']:
                time.sleep(.5)


def run():
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    url = f'http://127.0.0.1:{server.server_address[1]}/#{TOKEN}'
    threading.Thread(target=server.serve_forever, daemon=True).start()
    STATE.rescan()
    if os.environ.get('ECHOQUEUE_NO_WINDOW'):   # tests: serve only, print where
        print(url, flush=True)
        threading.Event().wait()
    if own_window(url):
        server.shutdown()
        return
    browser = edge()
    if not browser:                             # no Edge: the default browser; the installer stays until closed
        import webbrowser
        webbrowser.open(url)
        threading.Event().wait()
    profile = Path(os.environ.get('LOCALAPPDATA', '.')) / 'EchoLazyTablet' / 'window'
    profile.mkdir(parents=True, exist_ok=True)
    window = subprocess.Popen([browser, f'--app={url}', f'--user-data-dir={profile}', '--window-size=1000,780',
                               '--no-first-run', '--no-default-browser-check'])
    # Edge may hand the window to an Edge already running with this profile (an earlier
    # installer's window) and exit at once, so the window's own process says nothing about
    # whether it's open: the installer stays while the page keeps calling, and closes when the
    # page says goodbye as it closes, or 75 s after it stops calling (Edge slows the timers of
    # minimised windows to about one a minute); 60 s for the first call, while Edge starts.
    start = time.monotonic()
    while True:
        time.sleep(1)
        seen = LAST_SEEN
        if seen is None:
            if time.monotonic() - start > 60:
                break
        # a goodbye with no call after it: closed, not reloaded
        elif ((BYE_AT and seen < BYE_AT and time.monotonic() - BYE_AT > 2) or time.monotonic() - seen > 75) and not STATE.job['busy']:
            break
    window.poll()
    server.shutdown()
