"""EchoQueueInstaller.exe: opens the installer window (webapp.py), or without a window:

    EchoQueueInstaller.exe --scan|--install|--uninstall "<game folder>" "<report.txt>"

writes what happened to the report (for support, and for testing the built installer)."""
import sys
import traceback
from pathlib import Path

import patcher


def command_line(argv):
    action, folder, report = argv[:3]
    lines = []
    try:
        path = patcher.normalise(folder)
        if not path:
            raise patcher.InstallError(f'{folder} is not an Echo VR folder.')
        game = patcher.Game(path)
        if action == '--scan':
            info = patcher.scan(game)
            lines += [f"state: {info['state']}", f"tabs: {info.get('tabs')}", 'plugins: ' + ', '.join(info['plugins']),
                      f"echo mods: {info.get('echomods')}"]
            lines += ['note: ' + n for n in info['notes']] + ['problem: ' + p for p in info['problems']]
        else:
            (patcher.install if action == '--install' else patcher.uninstall)(game, log=lines.append)
            lines.append('OK')
        code = 0
    except Exception as e:
        lines.append(f'FAILED: {e}' if isinstance(e, patcher.InstallError) else 'FAILED:\n' + traceback.format_exc())
        code = 1
    Path(report).write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return code


def main():
    if len(sys.argv) >= 4 and sys.argv[1] in ('--scan', '--install', '--uninstall'):
        sys.exit(command_line(sys.argv[1:]))
    import webapp
    webapp.run()


if __name__ == '__main__':
    main()
