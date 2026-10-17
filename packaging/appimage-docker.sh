#!/usr/bin/env bash
# Builds dist/SacredFire-x86_64.AppImage (Steam Deck) in a nixos/nix Docker container, for hosts
# without Nix: packaging/appimage.sh's steps (build.sh inside nix-shell, then nix bundle) with the
# Nix store kept in the Docker volume nixstore-ctr between runs. The everyday build in out/ is
# moved to out.arch meanwhile and put back afterwards (the Nix build needs its own out/).
#   bash packaging/appimage-docker.sh            # nixpkgs: nixos-unstable (NIXPKGS=... another)
# Needs Docker usable by this user; the checkout must be committed (appimage.sh's check;
# BB_ALLOW_DIRTY=1 for a test build).
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
root=$PWD
command -v docker >/dev/null || { echo 'Needs Docker.' >&2; exit 1; }
if [[ -e out.arch ]]; then echo 'out.arch exists: put it back as out/ (or remove it) first.' >&2; exit 1; fi
restore() {
    if [[ -d out.arch ]]; then
        rm -rf out.nix
        [[ -d out ]] && mv out out.nix
        mv out.arch out
    fi
}
if [[ -d out ]]; then mv out out.arch; fi
[[ -d out.nix ]] && mv out.nix out  # an earlier Nix build: incremental
trap restore EXIT
script=$(mktemp)
cat > "$script" <<'EOF'
set -euxo pipefail
export NIX_CONFIG=$'experimental-features = nix-command flakes\nsandbox = false\nfilter-syscalls = false'
export NIX_PATH=nixpkgs=https://github.com/NixOS/nixpkgs/archive/${NIXPKGS:-nixos-unstable}.tar.gz
git config --global --add safe.directory '*'
cd /src
trap "chown -R ${HOST_UID}:${HOST_GID} /src" EXIT
nix-shell shell.nix --run 'BB_IN_NIX_SHELL=1 bash build.sh'
nix-shell shell.nix --run 'TMPDIR=/tmp bash packaging/appimage.sh'
EOF
docker run --rm -v "$root:/src" -v nixstore-ctr:/nix -v "$script:/build.sh:ro" \
    -e NIXPKGS="${NIXPKGS:-nixos-unstable}" -e BB_ALLOW_DIRTY="${BB_ALLOW_DIRTY:-}" \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" nixos/nix bash /build.sh
rm -f "$script"
ls -lh dist/SacredFire-x86_64.AppImage
cat dist/SacredFire-x86_64.AppImage.build.txt
