#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Links the online nickname to the player's Discord account (Linux, Steam Deck): a reinstall or
another PC recovers it. The port of the Windows launcher's discord_link.cpp and StartDiscordLink.

  discord_link.py status  [--config bbport.ini] [--data DIR]   the nickname's state on the server
  discord_link.py link    [...]   link online_name to the Discord account (browser)
  discord_link.py recover [...]   get the Discord account's nickname back on this installation
  discord_link.py unlink  [...]   unlink online_name from Discord

The browser opens Discord's authorization page (OAuth2, scope identify); Discord redirects it to
http://127.0.0.1:<port>/discord with a code, which only the RyuNET server (holding the
application's secret) can trade for the account. Every server request carries this
installation's key as run.py derives it (X-RyuNET-Key: HMAC-SHA256(key, host)). The outcome is
printed as one JSON line ({"ok":..,"status":..,"message":..,"nick":..}) and, with --result FILE,
written there (the in-game menu reads it). recover writes the nickname into bbport.ini.
"""
import argparse
import http.server
import json
import os
import secrets
import subprocess
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from run import online_key, server_key  # noqa: E402

# The Discord application of RyuNET (the Windows launcher's kDiscordAppId); its redirect URIs
# http://127.0.0.1:<port>/discord are registered for these ports.
DISCORD_APP_ID = '1560408801704943708'
PORTS = (53134, 53135, 53136)
DEFAULT_SERVER = 'octr.ryu7w7.xyz'


def ini_values(config):
    values = {}
    try:
        for line in Path(config).read_text(encoding='utf-8', errors='replace').splitlines():
            key, sep, value = line.partition('=')
            if sep and not key.startswith('#'):
                values[key.strip()] = value.strip()
    except OSError:
        pass
    return values


def set_ini(config, key, value):
    """key=value in bbport.ini, replacing the line or adding it."""
    path = Path(config)
    lines = path.read_text(encoding='utf-8', errors='replace').splitlines() if path.exists() else []
    done = False
    for i, line in enumerate(lines):
        if line.partition('=')[0].strip() == key:
            lines[i] = f'{key}={value}'
            done = True
    if not done:
        lines.append(f'{key}={value}')
    path.write_text('\n'.join(lines) + '\n', encoding='utf-8')


def server_address(values):
    """(host, http port) as run.py setup_online reads them."""
    server = (os.environ.get('BB_ONLINE_SERVER') or values.get('online_server') or DEFAULT_SERVER).strip()
    port = values.get('online_http_port', '8080')
    host = server
    if ':' in server and not server.startswith('['):
        h, _, p = server.partition(':')
        if p.isdigit():
            host, port = h, p
    return host, int(port)


def request(host, port, key, method, path, form=None, timeout=25):
    """The server's JSON answer as a dict ({} when it cannot be reached or answers no JSON)."""
    data = urllib.parse.urlencode(form).encode() if form is not None else None
    req = urllib.request.Request(f'http://{host}:{port}{path}', data=data, method=method)
    req.add_header('User-Agent', 'CTR-NitroFueled-PC launcher')
    if key:
        req.add_header('X-RyuNET-Key', key)
    if data is not None:
        req.add_header('Content-Type', 'application/x-www-form-urlencoded')
    try:
        with urllib.request.urlopen(req, timeout=timeout) as answer:
            body = answer.read(4096)
    except urllib.error.HTTPError as e:
        body = e.read(4096)
    except (OSError, ValueError):
        return {}
    try:
        return json.loads(body.decode('utf-8', 'replace'))
    except ValueError:
        return {}


def page(title, text):
    return ('<!doctype html><html><head><meta charset="utf-8"><title>' + title + '</title></head>'
            '<body style="font-family:sans-serif;background:#1e1f22;color:#eee;display:flex;align-items:center;'
            'justify-content:center;height:100vh;margin:0"><div style="text-align:center"><h2>' + title +
            '</h2><p>' + text + '</p></div></body></html>').encode()


def open_browser(url):
    """The desktop's browser (xdg-open; on the Steam Deck in desktop mode as well)."""
    for command in (['xdg-open', url], ['gio', 'open', url]):
        try:
            subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
                             env={k: v for k, v in os.environ.items() if k not in ('LD_LIBRARY_PATH', 'LD_PRELOAD',
                                                                                     'PYTHONPATH', 'PYTHONHOME')})
            return
        except OSError:
            continue
    webbrowser.open(url)


def authorize(timeout=180):
    """Discord's authorization in the browser: {'code':..,'redirect_uri':..} or {'error':..}."""
    state = secrets.token_hex(16)
    outcome = {}
    done = threading.Event()

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def reply(self, code, title, text):
            body = page(title, text)
            self.send_response(code)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            url = urllib.parse.urlparse(self.path)
            if url.path != '/discord':
                self.reply(404, '404', '')
                return
            query = urllib.parse.parse_qs(url.query)
            if query.get('state', [''])[0] != state:  # an old tab, another program
                self.reply(400, 'Invalid request', "This authorization is not the game's.")
                return
            code, error = query.get('code', [''])[0], query.get('error', [''])[0]
            if error or not code:
                outcome['error'] = 'denied'
                self.reply(200, 'Authorization cancelled', 'You can close this tab.')
            else:
                outcome['code'] = code
                self.reply(200, 'Done', 'Discord account authorized. You can close this tab and go back to the game.')
            done.set()

    server = None
    for port in PORTS:
        try:
            server = http.server.HTTPServer(('127.0.0.1', port), Handler)
            break
        except OSError:
            continue
    if server is None:
        return {'error': 'busy'}
    redirect = f'http://127.0.0.1:{server.server_address[1]}/discord'
    url = ('https://discord.com/oauth2/authorize?' + urllib.parse.urlencode({
        'client_id': DISCORD_APP_ID, 'response_type': 'code', 'scope': 'identify', 'prompt': 'none',
        'redirect_uri': redirect, 'state': state}))
    server.timeout = 1
    open_browser(url)
    waited = 0
    while not done.is_set() and waited < timeout:
        server.handle_request()
        waited += 1
    server.server_close()
    if not done.is_set():
        return {'error': 'timeout'}
    return dict(outcome, redirect_uri=redirect)


MESSAGES = {
    'linked': 'Done: {nick} is linked to your Discord account {discord}. After a reinstall or on another PC, '
              'use Recover with Discord.',
    'recovered': 'Recovered: {nick} is yours again on this installation (Discord: {discord}).',
    'own_nick': 'This installation already has the nickname {nick}, not linked to Discord: link it to recover '
                'it on any PC.',
    'no_nick': 'Your Discord account {discord} has no nickname linked on this server: link one first.',
    'other_nick': 'Your Discord account already has the nickname {nick}: one per account.',
    'taken': 'That nickname belongs to someone else or to another Discord account.',
    'invalid': 'The server did not accept the request.',
    'reserved': 'The server did not accept the request.',
}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('command', choices=('status', 'link', 'recover', 'unlink'))
    data_default = Path(os.environ.get('BB_DATA_DIR', ROOT))
    p.add_argument('--data', type=Path, default=data_default, help='the folder of ryunet_online.key and bbport.ini')
    p.add_argument('--config', type=Path, help='bbport.ini (default: DATA/bbport.ini, or BB_CONFIG)')
    p.add_argument('--result', type=Path, help='also write the JSON outcome to this file')
    a = p.parse_args()
    config = a.config or Path(os.environ.get('BB_CONFIG') or a.data / 'bbport.ini')
    values = ini_values(config)
    host, port = server_address(values)
    raw_key = online_key(a.data)
    key = server_key(raw_key, host) if raw_key else ''
    nick = (os.environ.get('BB_ONLINE_NAME') or values.get('online_name', '')).strip()

    result = {'ok': False, 'command': a.command, 'nick': nick}
    if a.command == 'status':
        body = request(host, port, key, 'GET', '/ryunet/nick?name=' + urllib.parse.quote(nick), timeout=10)
        result.update(ok=bool(body), status=body.get('status', ''), linked=bool(body.get('linked')),
                      discord=body.get('discord', ''),
                      message=('Linked to Discord ' + body.get('discord', '') if body.get('linked')
                               else 'Not linked to Discord' if body else 'Could not reach the server.'))
    elif a.command == 'unlink':
        body = request(host, port, key, 'POST', '/ryunet/discord/unlink', {'nick': nick}, timeout=10)
        result.update(ok=bool(body.get('unlinked')),
                      message='Nickname unlinked from Discord.' if body.get('unlinked') else 'Could not unlink.')
    else:
        if a.command == 'link' and not nick:
            result['message'] = 'Choose a nickname first (online_name).'
        else:
            auth = authorize()
            if 'error' in auth:
                result['message'] = {
                    'denied': 'You cancelled the authorization in Discord.',
                    'busy': 'Ports 53134 to 53136 are in use: close other launchers.',
                    'timeout': "Discord's authorization did not arrive in time (3 minutes).",
                }.get(auth['error'], 'Could not authorize with Discord.')
            else:
                body = request(host, port, key, 'POST', '/ryunet/discord/link',
                               {'nick': nick if a.command == 'link' else '', 'code': auth['code'],
                                'redirect_uri': auth['redirect_uri']})
                status = body.get('status', '')
                got = body.get('nick') or nick
                result.update(status=status, discord=body.get('discord', ''),
                              ok=status in ('linked', 'recovered', 'own_nick'))
                if status in ('recovered', 'own_nick') and got:
                    set_ini(config, 'online_name', got)
                    result['nick'] = got
                result['message'] = (MESSAGES.get(status, '').format(nick=got, discord=body.get('discord', ''))
                                     or ('Could not reach the server.' if not body else
                                         'The server could not confirm the account with Discord. Try again.'))
    line = json.dumps(result)
    print(line)
    if a.result:
        try:
            a.result.write_text(line + '\n', encoding='utf-8')
        except OSError:
            pass
    return 0 if result['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
