"""Non-Steam shortcuts for SacredFire: Steam's userdata/<user>/config/shortcuts.vdf.

Steam keeps its non-Steam games in a binary VDF file that it rewrites when it exits, so an entry
added while Steam runs is lost: callers check `steam_running()` first. The file is a map of
numbered entries; each entry holds strings (0x01), 32-bit integers (0x02) and nested maps (0x00,
ended by 0x08). The app id of a shortcut is the CRC32 of its executable and name with the top bit
set, as Steam computes it (the same id names its grid artwork).
"""
import os
import struct
import subprocess
import zlib
from pathlib import Path

NAME = "SacredFire"


def steam_roots():
    """Steam installations of this user, existing ones only (symlinked roots resolve to one)."""
    home = Path.home()
    seen, roots = set(), []
    for candidate in (home / ".steam/steam", home / ".local/share/Steam", home / ".steam/root",
                      home / ".var/app/com.valvesoftware.Steam/.local/share/Steam"):
        if not (candidate / "userdata").is_dir():
            continue
        real = candidate.resolve()
        if real in seen:
            continue
        seen.add(real)
        roots.append(candidate)
    return roots


def shortcut_files():
    """shortcuts.vdf of every Steam user found (the file may not exist yet: the path is returned)."""
    files = []
    for root in steam_roots():
        for user in sorted((root / "userdata").iterdir()):
            if user.name.isdigit() and user.name != "0" and (user / "config").is_dir():
                files.append(user / "config/shortcuts.vdf")
    return files


def steam_running():
    try:
        return subprocess.run(["pgrep", "-x", "steam"], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0
    except OSError:
        return False


# --- binary VDF ----------------------------------------------------------------------------

def _read_string(data, at):
    end = data.index(b"\0", at)
    return data[at:end].decode("utf-8", "replace"), end + 1


def parse(data):
    """The file as nested dicts (keys and string values as str, integers as int)."""
    def read_map(at):
        result = {}
        while True:
            kind = data[at]
            at += 1
            if kind == 0x08:
                return result, at
            key, at = _read_string(data, at)
            if kind == 0x00:
                result[key], at = read_map(at)
            elif kind == 0x01:
                result[key], at = _read_string(data, at)
            elif kind == 0x02:
                result[key] = struct.unpack_from("<i", data, at)[0]
                at += 4
            else:
                raise ValueError(f"unknown VDF field type {kind:#x} at {at - 1}")
    if not data:
        return {"shortcuts": {}}
    top, _ = read_map(0)
    return top


def serialize(top):
    out = bytearray()

    def write_map(mapping):
        nonlocal out
        for key, value in mapping.items():
            if isinstance(value, dict):
                out.append(0x00)
                out += key.encode() + b"\0"
                write_map(value)
            elif isinstance(value, int):
                out.append(0x02)
                out += key.encode() + b"\0" + struct.pack("<i", value)
            else:
                out.append(0x01)
                out += key.encode() + b"\0" + str(value).encode() + b"\0"
        out.append(0x08)
    write_map(top)
    return bytes(out)


def shortcut_appid(exe, name):
    """Steam's id for a non-Steam shortcut, as a signed 32-bit value (the file stores it so)."""
    value = (zlib.crc32((exe + name).encode()) | 0x80000000) & 0xFFFFFFFF
    return value - (1 << 32) if value >= 1 << 31 else value


def make_entry(exe, start_dir, name=NAME, launch_options="", icon=""):
    quoted = f'"{exe}"'
    return {
        "appid": shortcut_appid(quoted, name),
        "AppName": name,
        "Exe": quoted,
        "StartDir": f'"{start_dir}"',
        "icon": icon,
        "ShortcutPath": "",
        "LaunchOptions": launch_options,
        "IsHidden": 0,
        "AllowDesktopConfig": 1,
        "AllowOverlay": 1,
        "OpenVR": 0,
        "Devkit": 0,
        "DevkitGameID": "",
        "DevkitOverrideAppID": 0,
        "LastPlayTime": 0,
        "FlatpakAppID": "",
        "tags": {},
    }


def add_shortcut(path, entry):
    """Adds `entry` to the shortcuts file at `path` (created if missing); an entry with the same
    executable and name is replaced. Returns True when a new entry was added, False on replace."""
    path = Path(path)
    data = path.read_bytes() if path.exists() else b""
    top = parse(data)
    shortcuts = top.setdefault("shortcuts", {})
    for key, existing in list(shortcuts.items()):
        if isinstance(existing, dict) and existing.get("Exe") == entry["Exe"] \
                and existing.get("AppName") == entry["AppName"]:
            shortcuts[key] = entry
            added = False
            break
    else:
        index = max((int(k) for k in shortcuts if k.isdigit()), default=-1) + 1
        shortcuts[str(index)] = entry
        added = True
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + ".tmp")
    temp.write_bytes(serialize(top))
    os.replace(temp, path)
    return added


def launcher_command():
    """What Steam should start: the AppImage when running from one, else launcher/bb-launcher.sh;
    `--play` starts the game without the launcher window (game mode)."""
    appimage = os.environ.get("APPIMAGE")
    if appimage and Path(appimage).is_file():
        exe = str(Path(appimage).resolve())
    else:
        exe = str((Path(__file__).resolve().parent / "bb-launcher.sh"))
    return exe, str(Path(exe).parent), "--play"


# --- command line: install-desktop.sh adds the game to Steam (Steam Deck game mode) ----------

def grid_artwork(path, appid, icon="", hero=""):
    """Library artwork for a shortcut: config/grid/<appid>p.png (portrait capsule), <appid>.png
    (wide capsule), <appid>_hero.png; Steam crops what does not fit."""
    grid = Path(path).parent / "grid"
    grid.mkdir(parents=True, exist_ok=True)
    unsigned = appid & 0xFFFFFFFF
    copies = [(icon, f"{unsigned}p.png"), (hero or icon, f"{unsigned}.png"), (hero, f"{unsigned}_hero.png")]
    for source, name in copies:
        if source and Path(source).is_file():
            (grid / name).write_bytes(Path(source).read_bytes())


def add_everywhere(exe, name, icon="", hero=""):
    """The shortcut (and artwork) for every Steam user; returns (added, updated) counts."""
    entry = make_entry(exe, str(Path(exe).parent), name=name, icon=icon)
    added = updated = 0
    for path in shortcut_files():
        if add_shortcut(path, entry):
            added += 1
        else:
            updated += 1
        grid_artwork(path, entry["appid"], icon, hero)
    return added, updated


def main(argv):
    import argparse
    import time
    p = argparse.ArgumentParser(description="Adds a non-Steam shortcut for every Steam user.")
    p.add_argument("--exe", required=True)
    p.add_argument("--name", default=NAME)
    p.add_argument("--icon", default="")
    p.add_argument("--hero", default="")
    p.add_argument("--wait", action="store_true",
                   help="if Steam runs, wait (up to a day) until it exits, then add the shortcut")
    a = p.parse_args(argv)
    if not shortcut_files():
        print("Steam: no Steam user found; no shortcut added")
        return 0
    if steam_running():
        if not a.wait:
            print("Steam: running; close it and run again (it overwrites its shortcuts when it exits)")
            return 1
        deadline = time.time() + 86400
        while steam_running() and time.time() < deadline:
            time.sleep(10)
        time.sleep(3)  # let Steam finish writing its files
        if steam_running():
            return 1
    added, updated = add_everywhere(a.exe, a.name, a.icon, a.hero)
    print(f'Steam: "{a.name}" added for {added} user(s), updated for {updated}; it shows after Steam starts')
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main(sys.argv[1:]))
