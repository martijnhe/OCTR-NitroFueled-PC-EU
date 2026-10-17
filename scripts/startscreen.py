#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The start screen's hidden parts (TODO.md, Grand Prix intros): the 1.21 start screen
(maps/Octane/Octane_StartScreen/Octane_StartScreen.igz in the update's archives/update2.pak)
still holds entities its _canSpawn bit (CGameEntity +0x38, bit 0) keeps out since the Grand Prix
system ended. This writes, from the player's own update2.pak, a byte overlay that the runtime
serves in place of the map's compressed pieces (BB_FILE_OVERLAY, src/runtime_file.c): the game
file itself is not changed.

  startscreen.py --game GAME --out FILE [--tga] [--gp8] [--gp7]

--tga  The Game Awards 2019 statue (Trophy_TGA) in place of the cup (Trophy), as in 1.18.
--gp8  The Gasmoxia Grand Prix guests: GrandPrixGuest (Cortex) and GrandPrixGuest2 (Nitros Oxide).
--gp7  The Rustland Grand Prix embers (gp7_embers_vfx, 001, 002).

Overlay format: "BBOVL1\\n", then records: u16 guest path length, the guest path
(/app0/archives/update2.pak), u64 file offset, u32 size, the bytes. No option: no file.
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import iga  # noqa: E402

MAP = 'maps/Octane/Octane_StartScreen/Octane_StartScreen.igz'
ARCHIVE = 'archives/update2.pak'
CAN_SPAWN = 0x38  # CGameEntity bitfield; bit 0: _canSpawn
CHOICES = {
    'tga': {'Trophy_TGA': 1, 'Trophy': 0},
    'gp8': {'GrandPrixGuest': 1, 'GrandPrixGuest2': 1},
    'gp7': {'gp7_embers_vfx': 1, 'gp7_embers_vfx001': 1, 'gp7_embers_vfx002': 1},
}


def overlay(game, chosen):
    """[(file offset, bytes)] that turn the chosen parts on."""
    archive = iga.Archive(Path(game) / ARCHIVE)
    data = bytearray(archive.read(MAP))
    objects = iga.Objects(bytes(data))
    names = objects.named()
    for choice in chosen:
        for name, on in CHOICES[choice].items():
            if name not in names or objects.type_of(names[name]) != 'CGameEntity':
                raise ValueError(f'the start screen has no entity {name} (another game version?)')
            at = objects.base + names[name] + CAN_SPAWN
            data[at] = (data[at] & ~1) | on
    original = archive.read(MAP)
    out = []
    with open(archive.path, 'rb') as f:
        for k, (position, stored, room, packed, size) in enumerate(archive.pieces(MAP)):
            piece = bytes(data[k * iga.PIECE:k * iga.PIECE + size])
            if piece == original[k * iga.PIECE:k * iga.PIECE + size]:
                continue
            if not packed:
                out.append((position, piece))
                continue
            f.seek(position + 4)
            props = f.read(5)
            new = iga.pack_piece(props, piece)
            if len(new) > room:
                raise ValueError(f'the changed piece {k} does not fit ({len(new)} > {room} bytes)')
            out.append((position, new))
    # What the game will read: the changed map, decoded through the overlay.
    if archive.read(MAP, out) != bytes(data):
        raise ValueError('the overlay does not decode to the changed map')
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--game', required=True, type=Path)
    p.add_argument('--out', required=True, type=Path)
    for choice in CHOICES:
        p.add_argument(f'--{choice}', action='store_true')
    a = p.parse_args()
    chosen = [c for c in CHOICES if getattr(a, c)]
    if not chosen:
        a.out.unlink(missing_ok=True)
        return 0
    try:
        records = overlay(a.game, chosen)
    except (OSError, ValueError, KeyError) as e:
        print(f'Start screen: {e}; unchanged', file=sys.stderr)
        a.out.unlink(missing_ok=True)
        return 1
    with open(a.out, 'wb') as w:
        w.write(b'BBOVL1\n')
        path = ('/app0/' + ARCHIVE).encode()
        for offset, data in records:
            w.write(struct.pack('<H', len(path)) + path + struct.pack('<QI', offset, len(data)) + data)
    print(f'Start screen: {", ".join(chosen)} ({sum(len(d) for _, d in records)} bytes over {ARCHIVE})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
