#!/usr/bin/env bash
# Starts the game directly (no launcher window): the application menu entry "Crash Team Racing
# Nitro-Fueled" (install-desktop.sh) runs this. The game folder: $1, else BB_GAME_DIR, else the
# one install-desktop.sh saved in ~/.config/sacredfire/game_dir. Output goes to logs/last_play.log.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")/.." && pwd)
saved=${XDG_CONFIG_HOME:-$HOME/.config}/sacredfire/game_dir
game=${1:-${BB_GAME_DIR:-}}
if [[ -z $game && -f $saved ]]; then game=$(<"$saved"); fi
if [[ -z $game || ! -f $game/eboot.bin ]]; then
    msg="No game folder: run launcher/install-desktop.sh GAME_DIR (the merged game, eboot.bin in it)."
    command -v notify-send >/dev/null && notify-send "Crash Team Racing Nitro-Fueled" "$msg"
    echo "$msg" >&2
    exit 1
fi
mkdir -p "$here/logs"
cd "$here"
BB_GAME_DIR=$game exec bash run.sh "${@:2}" > "$here/logs/last_play.log" 2>&1
