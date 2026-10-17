#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The start screen's hidden parts (TODO.md, Grand Prix intros): the 1.21 start screen
(maps/Octane/Octane_StartScreen/Octane_StartScreen.igz in the update's archives/update2.pak)
still holds entities its _canSpawn bit (CGameEntity +0x38, bit 0) keeps out since the Grand Prix
system ended. This writes, from the player's own update2.pak, a byte overlay that the runtime
serves in place of the map's compressed pieces (BB_FILE_OVERLAY, src/runtime_file.c): the game
file itself is not changed.

  startscreen.py --game GAME --out FILE [--tga] [--gp8] [--gp7] [--gp4 --archive-out PAK]

--tga  The Game Awards 2019 statue (Trophy_TGA) in place of the cup (Trophy), as in 1.18.
--gp8  The Gasmoxia Grand Prix guests: GrandPrixGuest (Cortex) and GrandPrixGuest2 (Nitros Oxide).
--gp7  The Rustland Grand Prix embers (gp7_embers_vfx, 001, 002).
--gp4  The Spooky Grand Prix guest, Nina Cortex, as GrandPrixGuest (in place of Cortex with
       --gp8). Her files are only in the second start screen set (archives/Octane_StartScreen_Shooting.pak
       in update2.pak): PAK gets only her files, for the runtime to serve as archives/update3.pak
       (BB_FILE_REPLACE), an update archive 1.21 looks for at start and does not have (1.21 reads
       its start screen from the update archives, never from archives/octane_startscreen.pak).

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
    # A guest takes Crash's place on the logo (the seasons' hook that pulled him away is not in 1.21).
    'gp8': {'GrandPrixGuest': 1, 'GrandPrixGuest2': 1, 'Crash': 0},
    'gp7': {'gp7_embers_vfx': 1, 'gp7_embers_vfx001': 1, 'gp7_embers_vfx002': 1},
    'gp4': {'GrandPrixGuest': 1, 'Crash': 0},
}


GUEST_SKIN = 'CutsceneCharacter\\CutsceneDriverKCNGP8_P1'
NINA_SKIN = 'CutsceneCharacter\\CutsceneDriverNCX'
START_ARCHIVE = 'archives/octane_startscreen.pak'
SHOOTING_ARCHIVE = 'archives/Octane_StartScreen_Shooting.pak'
# Nina's files in the Shooting set (its package, packages/generated/maps/Octane_StartScreen_Shooting/
# Octane_StartScreen_Shooting_pkg.igz, lists them): her actor, animations, materials and textures.
NINA_FILES = ('actors/cutscenecharacter/cutscenedriverncx.igz', 'anims/cutscenecharacter/cutscenedriverncx.hka',
              'materialinstances/actors/octane/characters/gp4_driver_ncx_materials,cutscenedriverncx,ff.igz',
              'materialinstances/models/props/item_cup_race_materials,cutscenedriverncx,7.igz')
NINA_TEXTURES = ('gp4_driver_ncx', '{default_b`png,common!g_45`tga,default_m`png}')

# The start screen's package lists the files its map loads: the Gasmoxia guest's (Cortex, KCNGP8).
# With --gp4 its entries become Nina's, each for a file of the same kind (the package refers to
# its strings by index); Cortex's remaining texture entries stay (harmless).
PACKAGE = 'packages/generated/maps/Octane/Octane_StartScreen/Octane_StartScreen_pkg.igz'
_KCN = 'actors!octane!characters!gp8_skin_kcn_p1!gp8_kcn_'
_NCX = 'actors!octane!characters!gp4_driver_ncx!ncx_ncx_'
NINA_PACKAGE = (
    ('actors/cutscenecharacter/cutscenedriverkcngp8_p1.igz', 'actors/cutscenecharacter/cutscenedriverncx.igz'),
    ('anims/cutscenecharacter/cutscenedriverkcngp8_p1.hka', 'anims/cutscenecharacter/cutscenedriverncx.hka'),
    ('materialinstances/actors/octane/characters/gp8_skin_kcn_p1_materials,cutscenedriverkcngp8_p1,0a27.igz',
     'materialinstances/actors/octane/characters/gp4_driver_ncx_materials,cutscenedriverncx,ff.igz'),
    (f'textures/colormap,textures@!{_KCN}body_c`tga,111.igz', f'textures/colormap,textures@!{_NCX}body_c`tga,111.igz'),
    (f'textures/colormap,textures@!{_KCN}eyes_c`tga,111.igz', f'textures/colormap,textures@!{_NCX}cloth_c`tga,111.igz'),
    (f'textures/colormap,textures@!{_KCN}feathers_c`tga,111.igz', f'textures/colormap,textures@!{_NCX}hand_c`tga,111.igz'),
    (f'textures/colormap,textures@!{_KCN}fur_c`tga,111.igz',
     'textures/colormap,textures@!actors!octane!characters!gp4_driver_ncx!ncx_hairstrands_c`tif,111.igz'),
    (f'textures/composite3halfres,textures@!{{default_b`png,{_KCN}body_{{g,m}}`tga}},111.igz',
     f'textures/composite3halfres,textures@!{_NCX}body_{{b,g,m}}`tga,111.igz'),
    (f'textures/composite3halfres,textures@!{{default_b`png,{_KCN}eyes_{{g,m}}`tga}},111.igz',
     f'textures/composite3halfres,textures@!{_NCX}cloth_{{b,g,m}}`tga,111.igz'),
    (f'textures/composite3halfres,textures@!{{default_b`png,{_KCN}feathers_{{g,m}}`tga}},111.igz',
     f'textures/composite3halfres,textures@!{{default_b`png,{_NCX}hand_{{g,m}}`tga}},111.igz'),
    (f'textures/composite3halfres,textures@!{{default_b`png,{_KCN}fur_g`tga,default_m`png}},111.igz',
     'textures/composite3halfres,textures@!{default_b`png,common!g_45`tga,default_m`png},111.igz'),
    (f'textures/normalmap,textures@!{_KCN}body_n`tga,111.igz', f'textures/normalmap,textures@!{_NCX}body_n`tga,111.igz'),
    (f'textures/normalmap,textures@!{_KCN}eyes_n`tga,111.igz', f'textures/normalmap,textures@!{_NCX}cloth_n`tga,111.igz'),
    (f'textures/normalmap,textures@!{_KCN}feathers_n`tga,111.igz', f'textures/normalmap,textures@!{_NCX}hand_n`tga,111.igz'),
)

# The start screen's background: unlit materials drawing a blue gradient texture (the full-screen
# "event" one and player 1's), each with a colour multiplier (ig_color_value, white). With --gp4 the
# multiplier tints the blue to the Spooky season's teal: SPOOKY_TINT, from the reference recording's
# background over ours at the same spots (median per channel), to the power 1/2.4: on screen the
# tint acts about as tint^2.4 (measured: x1.37 doubled green, x0.71 cut blue to 0.42).
_BACKGROUND = 'materialinstances/Octane/M101_Customization/M101_Customization_background_materials,'
BACKGROUND_MATERIALS = (_BACKGROUND + 'octane_ui_custo_bg_color_event,008000.igz',
                        _BACKGROUND + 'octane_ui_custo_bg_color_1p,000800.igz')
SPOOKY_TINT = (1.09, 1.14, 0.87, 1.0)


def background(archive, tint):
    """[(file offset, bytes)]: the background materials' colour multiplier set to tint (RGBA)."""
    white = struct.pack('<4f', 1.0, 1.0, 1.0, 1.0)
    records = []
    for material in BACKGROUND_MATERIALS:
        data = archive.read(material)
        if data.count(white) != 1:
            raise ValueError(f'{material}: unexpected contents (another game version?)')
        records += iga.replacement(archive, material, data.replace(white, struct.pack('<4f', *tint)))
    return records


GUI_DATA = 'loosedata/GuiSystemData.igz'
START_MAP = 'Octane/Octane_StartScreen/Octane_StartScreen'
SHOOTING_MAP = 'Octane_StartScreen_Shooting/Octane_StartScreen_Shooting'


def shooting(game):
    """[(file offset, bytes)] that make the game start in the second start screen set
    (Octane_StartScreen_Shooting, in update2.pak's archives/Octane_StartScreen_Shooting.pak): its
    map name in place of the start screen's in loosedata/GuiSystemData.igz."""
    archive = iga.Archive(Path(game) / ARCHIVE)
    return iga.replacement(archive, GUI_DATA, iga.replace_string(archive.read(GUI_DATA), START_MAP, SHOOTING_MAP))


def nina_archive(game, out, work):
    """Writes out: an archive of Nina's files from the Shooting set (extracted from update2.pak
    to work meanwhile)."""
    update = iga.Archive(Path(game) / ARCHIVE)
    with open(work, 'wb') as w:
        w.write(update.read(SHOOTING_ARCHIVE))
    try:
        shooting = iga.Archive(work)
        names = [n for k, (_o, _l, _b, n) in shooting.files.items()
                 if k in NINA_FILES or (k.startswith('textures/') and any(t in k for t in NINA_TEXTURES))]
        if len(names) < len(NINA_FILES) + 10:
            raise ValueError(f'{SHOOTING_ARCHIVE}: Nina\'s files not found (another game version?)')
        iga.build(out, [(shooting, n) for n in names])
        check = iga.Archive(out)
        if any(check.read(n) != shooting.read(n) for n in names):
            raise ValueError(f'{out}: does not read back')
    finally:
        Path(work).unlink(missing_ok=True)


def overlay(game, chosen):
    """[(file offset, bytes)] that turn the chosen parts on."""
    archive = iga.Archive(Path(game) / ARCHIVE)
    data = bytearray(archive.read(MAP))
    if 'gp4' in chosen:
        data = bytearray(iga.replace_string(bytes(data), GUEST_SKIN, NINA_SKIN))
        chosen = [c for c in chosen if c != 'gp4'] + ['gp4']
    objects = iga.Objects(bytes(data))
    names = objects.named()
    for choice in chosen:
        for name, on in CHOICES[choice].items():
            if name not in names or objects.type_of(names[name]) != 'CGameEntity':
                raise ValueError(f'the start screen has no entity {name} (another game version?)')
            at = objects.base + names[name] + CAN_SPAWN
            data[at] = (data[at] & ~1) | on
    out = iga.replacement(archive, MAP, bytes(data))
    if 'gp4' in chosen:
        package = archive.read(PACKAGE)
        for old, new in NINA_PACKAGE:
            package = iga.replace_string(package, old, new)
        out += iga.replacement(archive, PACKAGE, package)
        out += background(archive, SPOOKY_TINT)
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--game', required=True, type=Path)
    p.add_argument('--out', required=True, type=Path)
    for choice in CHOICES:
        p.add_argument(f'--{choice}', action='store_true')
    # Research only (TODO.md): the set loads, but has no start-screen camera (a capture set).
    p.add_argument('--shooting', action='store_true', help='start in the second start screen set (research)')
    p.add_argument('--archive-out', type=Path, help='with --gp4: the start screen archive to write')
    a = p.parse_args()
    if a.gp4 and not a.archive_out:
        p.error('--gp4 needs --archive-out')
    chosen = [c for c in CHOICES if getattr(a, c)]
    if not chosen and not a.shooting:
        a.out.unlink(missing_ok=True)
        return 0
    try:
        records = (overlay(a.game, chosen) if chosen else []) + (shooting(a.game) if a.shooting else [])
        if a.gp4:
            nina_archive(a.game, a.archive_out, a.archive_out.with_suffix('.tmp'))
    except (OSError, ValueError, KeyError) as e:
        print(f'Start screen: {e}; unchanged', file=sys.stderr)
        a.out.unlink(missing_ok=True)
        if a.archive_out:
            a.archive_out.unlink(missing_ok=True)
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
