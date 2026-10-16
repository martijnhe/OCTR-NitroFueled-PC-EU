#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Discord Rich Presence and invites while the game runs (Linux, Steam Deck): the port of the
Windows launcher's discord_presence.cpp, UpdateDiscord and OnDiscordJoin.

run.sh starts it next to the game (online on, bbport.ini discord_rpc not 0) with --pid of the
game; it ends with the game. It talks to the Discord app over its IPC socket
($XDG_RUNTIME_DIR/discord-ipc-N, or Flatpak Discord's app/com.discordapp.Discord/discord-ipc-N),
retrying while Discord is closed.

- Presence: "Playing online", the nickname, the time played, RyuNET's art, and a party: Discord
  then shows "Join" and the chat's "+ > Invite to play". The join secret is
  "ctr1|<server>|<nickname>", as the Windows launcher writes it, so invites work between both.
- Joining: an invite accepted in Discord hands the secret here. The host becomes a friend
  (online_friends in bbport.ini) and ryunet_join.txt next to bbport.ini tells the game, which joins
  the host's private room as it would after a PS4 invitation (src/runtime_services.c). An invite
  to another server is ignored (a crafted invite must not move the player to any server).
- "Ask to Join" requests are accepted (bbport.ini discord_accept_requests=0: no).
"""
import argparse
import json
import os
import socket
import struct
import sys
import time
import urllib.parse
import urllib.request
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from run import nick_ok, online_key, server_key  # noqa: E402
sys.path.insert(0, str(ROOT / 'scripts'))
from discord_link import DISCORD_APP_ID, ini_values, server_address, set_ini  # noqa: E402

OP_HANDSHAKE, OP_FRAME, OP_CLOSE, OP_PING, OP_PONG = 0, 1, 2, 3, 4


def log(text):
    print(f'Discord presence: {text}', flush=True)


def sockets():
    """Candidate IPC socket paths, in Discord's own order."""
    bases = [os.environ.get(v) for v in ('XDG_RUNTIME_DIR', 'TMPDIR', 'TMP', 'TEMP')] + ['/tmp']
    out = []
    for base in filter(None, bases):
        for sub in ('', 'app/com.discordapp.Discord/', 'snap.discord/', '.flatpak/com.discordapp.Discord/xdg-run/'):
            for i in range(10):
                path = os.path.join(base, sub, f'discord-ipc-{i}')
                if path not in out:
                    out.append(path)
    return out


class Ipc:
    def __init__(self):
        self.sock = None
        self.buffer = b''

    def connect(self, app_id):
        for path in sockets():
            if not os.path.exists(path):
                continue
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                s.settimeout(5)
                s.connect(path)
                self.sock = s
                self.send(OP_HANDSHAKE, {'v': 1, 'client_id': app_id})
                op, data = self.receive(timeout=5)
                if op == OP_FRAME and data.get('evt') == 'READY':
                    user = data.get('data', {}).get('user', {})
                    log(f"connected ({path}, Discord user {user.get('username', '?')})")
                    self.sock.settimeout(None)
                    return True
                s.close()
            except OSError:
                s.close()
            self.sock = None
        return False

    def send(self, op, payload):
        data = json.dumps(payload).encode()
        self.sock.sendall(struct.pack('<II', op, len(data)) + data)

    def receive(self, timeout=None):
        """(op, payload) or (None, None) when nothing arrived within timeout."""
        self.sock.settimeout(timeout)
        try:
            while len(self.buffer) < 8 or len(self.buffer) < 8 + struct.unpack('<II', self.buffer[:8])[1]:
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise ConnectionError('Discord closed the connection')
                self.buffer += chunk
        except socket.timeout:
            return None, None
        op, size = struct.unpack('<II', self.buffer[:8])
        data, self.buffer = self.buffer[8:8 + size], self.buffer[8 + size:]
        return op, json.loads(data or b'{}')

    def command(self, cmd, args=None, evt=None):
        payload = {'cmd': cmd, 'nonce': str(uuid.uuid4())}
        if args is not None:
            payload['args'] = args
        if evt:
            payload['evt'] = evt
        self.send(OP_FRAME, payload)

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None
        self.buffer = b''


def party_info(host, port, key, nick):
    """(players, max) of the nickname's room on the server, or None."""
    url = f'http://{host}:{port}/ryunet/party?name=' + urllib.parse.quote(nick)
    req = urllib.request.Request(url, headers={'X-RyuNET-Key': key} if key else {})
    try:
        with urllib.request.urlopen(req, timeout=5) as answer:
            body = json.loads(answer.read(4096).decode('utf-8', 'replace'))
        return int(body['players']), int(body['max'])
    except (OSError, ValueError, KeyError, TypeError):
        return None


def activity(values, started, party):
    """The presence for the current settings (online on)."""
    nick = values.get('online_name', '')
    host, _ = server_address(values)
    act = {'details': 'Playing online', 'timestamps': {'start': started},
           'assets': {'large_image': 'logo', 'large_text': 'Crash Team Racing Nitro-Fueled'}}
    if nick:
        act['state'] = 'Nickname: ' + nick
    if nick_ok(nick):
        players, most = party or (1, 8)  # a server without /ryunet/party: offer it anyway
        if 0 < players <= most:
            act['party'] = {'id': f'ctr:{host.lower()}:{nick.lower()}', 'size': [players, most]}
            if players < most:
                act['secrets'] = {'join': f'ctr1|{host}|{nick}'}
    return act


def accept_join(config, values, secret):
    """An invite or "Join" accepted in Discord: the host as a friend and ryunet_join.txt."""
    parts = secret.split('|')
    if len(parts) != 3 or parts[0] != 'ctr1' or not nick_ok(parts[2]):
        log('ignored an invite that is not from this game')
        return
    server, host_nick = parts[1], parts[2]
    my_host, _ = server_address(values)
    if host_nick.lower() == values.get('online_name', '').lower():
        return  # our own invite
    if server.strip().lower() != my_host.strip().lower():
        log(f'ignored an invite to another server ({server}); switch servers in bbport.ini to join it')
        return
    friends = [f.strip() for f in values.get('online_friends', '').split(',') if f.strip()]
    if host_nick.lower() not in (f.lower() for f in friends):
        set_ini(config, 'online_friends', ','.join(friends + [host_nick]))
    Path(config).with_name('ryunet_join.txt').write_text(
        f'session=cusa13795-session-{host_nick}\nhost={host_nick}\ntime={int(time.time())}\n', encoding='utf-8')
    log(f"{host_nick}'s invite accepted: the game joins their room in a few seconds")


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--pid', type=int, required=True, help='the game: the presence ends with it')
    p.add_argument('--data', type=Path, default=Path(os.environ.get('BB_DATA_DIR', ROOT)))
    p.add_argument('--config', type=Path)
    a = p.parse_args()
    config = a.config or Path(os.environ.get('BB_CONFIG') or a.data / 'bbport.ini')
    # One helper per game: a restart runs run.sh again in the same process.
    lock = Path(os.environ.get('XDG_RUNTIME_DIR') or '/tmp') / f'ctr-discord-presence-{a.pid}.lock'
    try:
        fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        os.write(fd, str(os.getpid()).encode())
        os.close(fd)
    except FileExistsError:
        try:
            if alive(int(lock.read_text())):
                return 0
        except (OSError, ValueError):
            pass
        lock.write_text(str(os.getpid()))
    started = int(time.time())
    ipc = Ipc()
    shown = None
    party, party_at = None, 0.0
    try:
        while alive(a.pid):
            values = ini_values(config)
            if values.get('discord_rpc', '1') == '0' or values.get('online', '0') != '1':
                ipc.close()
                time.sleep(5)
                continue
            if not ipc.sock:
                if not ipc.connect(DISCORD_APP_ID):
                    time.sleep(10)  # Discord not running: try again
                    continue
                for evt in ('ACTIVITY_JOIN', 'ACTIVITY_JOIN_REQUEST'):
                    ipc.command('SUBSCRIBE', {}, evt)
                shown = None
            host, port = server_address(values)
            nick = values.get('online_name', '')
            if time.time() - party_at > 20 and nick_ok(nick):
                raw = online_key(a.data)
                party = party_info(host, port, server_key(raw, host) if raw else '', nick)
                party_at = time.time()
            wanted = activity(values, started, party)
            try:
                if wanted != shown:
                    ipc.command('SET_ACTIVITY', {'pid': a.pid, 'activity': wanted})
                    shown = wanted
                op, data = ipc.receive(timeout=2)
                while op is not None:
                    if op == OP_PING:
                        ipc.send(OP_PONG, data)
                    elif op == OP_CLOSE:
                        raise ConnectionError(data.get('message', 'closed'))
                    elif op == OP_FRAME and data.get('evt') == 'ERROR':
                        log(f"Discord: {data.get('data', {}).get('message', 'error')}")
                    elif op == OP_FRAME and data.get('evt') == 'ACTIVITY_JOIN':
                        accept_join(config, values, data.get('data', {}).get('secret', ''))
                    elif op == OP_FRAME and data.get('evt') == 'ACTIVITY_JOIN_REQUEST':
                        user = data.get('data', {}).get('user', {})
                        if values.get('discord_accept_requests', '1') != '0' and user.get('id'):
                            ipc.command('SEND_ACTIVITY_JOIN_INVITE', {'user_id': user['id']})
                            log(f"accepted {user.get('username', '?')}'s request to join")
                    op, data = ipc.receive(timeout=0.1)
            except (OSError, ConnectionError, ValueError) as e:
                log(f'Discord connection lost ({e}); retrying')
                ipc.close()
                time.sleep(5)
    finally:
        if ipc.sock:
            try:
                ipc.command('SET_ACTIVITY', {'pid': a.pid})  # clear it
            except OSError:
                pass
        ipc.close()
        try:
            if lock.read_text() == str(os.getpid()):
                lock.unlink()
        except OSError:
            pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
