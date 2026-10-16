#!/usr/bin/env bash
# Install + run CTR Nitro-Fueled PC port (OCTR-NitroFueled-PC) on Arch/CachyOS.
# Usage:
#   ./nitrofueledpcscript.sh install                       # deps + clone + build
#   ./nitrofueledpcscript.sh merge <base> <update> <out>   # merge 1.21 update into base dump
#   ./nitrofueledpcscript.sh run <merged game dir>         # play
#   ./nitrofueledpcscript.sh dlc <folder with .pkg files>  # add your DLC packages (EU or US)
set -euo pipefail

REPO_URL="https://github.com/martijnhe/OCTR-NitroFueled-PC-EU"
# The checkout: INSTALL_DIR, else the repository this script is in, else ~/OCTR-NitroFueled-PC-EU.
here=$(cd -- "$(dirname -- "$0")" && pwd)
if [[ -z ${INSTALL_DIR:-} && -f $here/build.sh && -d $here/.git ]]; then INSTALL_DIR=$here; fi
INSTALL_DIR="${INSTALL_DIR:-$HOME/OCTR-NitroFueled-PC-EU}"

install() {
    echo "==> Installing build dependencies"
    sudo pacman -S --needed base-devel git cmake ninja python glslang sdl3 vulkan-headers vulkan-icd-loader \
        boost fmt robin-map xxhash libx11 zydis ffmpeg pkgconf

    if [[ -d "$INSTALL_DIR/.git" ]]; then
        echo "==> Updating existing checkout"
        git -C "$INSTALL_DIR" pull
        git -C "$INSTALL_DIR" submodule update --init --recursive
    else
        echo "==> Cloning repo"
        git clone --recursive "$REPO_URL" "$INSTALL_DIR"
    fi

    echo "==> Building"
    (cd "$INSTALL_DIR" && bash ./build.sh)
    echo "==> Application menu entries (Caelestia search: \"Crash Team Racing Nitro-Fueled\")"
    bash "$INSTALL_DIR/launcher/install-desktop.sh" ${GAME_DIR:+"$GAME_DIR"}
    echo "==> Done. Next: $0 merge <base> <update> <out>"
}

merge() {
    [[ $# -eq 3 ]] || { echo "Usage: $0 merge <base dir> <update dir> <output dir>"; exit 1; }
    python "$INSTALL_DIR/tools/merge_update.py" "$1" "$2" "$3"
    bash "$INSTALL_DIR/launcher/install-desktop.sh" "$3"   # the menu entry plays this folder
    echo "==> Merged. Next: $0 run \"$3\" (or search \"Crash Team Racing\" in the app launcher)"
}

dlc() {
    [[ $# -eq 1 ]] || { echo "Usage: $0 dlc <folder with DLC .pkg files>"; exit 1; }
    mkdir -p "$INSTALL_DIR/dlc"
    find "$1" -iname '*.pkg' -exec cp -v {} "$INSTALL_DIR/dlc/" \;
    echo "==> DLC packages copied; the game reports those of its own edition installed at the next start"
}

run() {
    [[ $# -eq 1 ]] || { echo "Usage: $0 run <merged game dir>"; exit 1; }
    cd "$INSTALL_DIR"
    BB_GAME_DIR="$(realpath "$1")" ./run.sh
}

case "${1:-}" in
    install) install ;;
    merge)   shift; merge "$@" ;;
    run)     shift; run "$@" ;;
    dlc)     shift; dlc "$@" ;;
    *) sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
