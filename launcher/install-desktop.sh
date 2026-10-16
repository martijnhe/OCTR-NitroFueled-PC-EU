#!/usr/bin/env bash
# Adds the launcher to the application menu (~/.local/share/applications) for this checkout, with
# the game's own icon (sce_sys/icon0.png from the game folder: $1, else the launcher's saved game
# folder, else orig/CUSA13795) copied into the user's icon theme. Game files never enter the repo.
# Also "Crash Team Racing Nitro-Fueled": starts the game directly (play.sh) with that game folder,
# saved in ~/.config/sacredfire/game_dir (app launchers such as Caelestia's search list it).
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
share=${XDG_DATA_HOME:-$HOME/.local/share}
dest=$share/applications
mkdir -p "$dest"
game=${1:-}
if [[ -z $game && -f ${XDG_CONFIG_HOME:-$HOME/.config}/sacredfire-launcher/settings.json ]]; then
    game=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("game_dir",""))' \
        "${XDG_CONFIG_HOME:-$HOME/.config}/sacredfire-launcher/settings.json" 2>/dev/null || true)
fi
[[ -n $game ]] || game=$here/../orig/CUSA13795
icon=applications-games
if [[ -f $game/sce_sys/icon0.png ]]; then
    mkdir -p "$share/icons/hicolor/512x512/apps"
    cp -f "$game/sce_sys/icon0.png" "$share/icons/hicolor/512x512/apps/io.github.sacredfire.Launcher.png"
    # By full path: launchers that read a stale icon-theme.cache (e.g. Caelestia) still find it.
    icon=$share/icons/hicolor/512x512/apps/io.github.sacredfire.Launcher.png
    command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -f -t "$share/icons/hicolor" >/dev/null 2>&1 || true
    echo "Icon: $game/sce_sys/icon0.png"
fi
sed -e "s|@LAUNCHER@|$here/bb-launcher.sh|" -e "s|@ICON@|$icon|" "$here/io.github.sacredfire.Launcher.desktop.in" \
    > "$dest/io.github.sacredfire.Launcher.desktop"
echo "Installed $dest/io.github.sacredfire.Launcher.desktop"
if [[ -f $game/eboot.bin ]]; then
    config=${XDG_CONFIG_HOME:-$HOME/.config}/sacredfire
    mkdir -p "$config"
    realpath -- "$game" > "$config/game_dir"
    sed -e "s|@PLAY@|$here/play.sh|" -e "s|@ICON@|$icon|" "$here/io.github.sacredfire.Play.desktop.in" \
        > "$dest/io.github.sacredfire.Play.desktop"
    echo "Installed $dest/io.github.sacredfire.Play.desktop (game: $(<"$config/game_dir"))"
else
    echo "No game folder with eboot.bin: the direct-play entry needs launcher/install-desktop.sh GAME_DIR"
fi
command -v update-desktop-database >/dev/null && update-desktop-database "$dest" 2>/dev/null || true
