#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds assets/button_icons/{xbox,deck,switch}.png, the button icon sheets scripts/buttons.py
puts in the game's fonts: each icon of Xelu's Free Controller Prompts (Those Awesome Guys,
https://thoseawesomeguys.com/prompts/, CC0) fitted into the rectangle the game's own PS4 icon
has in the fonts' button texture, the rest transparent (no game pixels in the sheets).
Needs Pillow; the game only to read the rectangles.

  button_icons.py --game GAME --prompts XELU_DIR    (XELU_DIR: the unpacked pack)
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import buttons  # noqa: E402
import iga  # noqa: E402

# Icon file per style for each PS4 button, by position (the bottom face button is the game's
# confirm: A on Xbox and the Deck, B on Switch).
ICONS = {
    'xbox': ('Xbox Series/XboxSeriesX_', {
        'cross': 'A', 'circle': 'B', 'triangle': 'Y', 'square': 'X', 'l1': 'LB', 'r1': 'RB',
        'l2': 'LT', 'r2': 'RT', 'l3': 'Left_Stick', 'r3': 'Right_Stick', 'touchpad': 'View'}),
    'deck': ('Steam Deck/SteamDeck_', {
        'cross': 'A', 'circle': 'B', 'triangle': 'Y', 'square': 'X', 'l1': 'L1', 'r1': 'R1',
        'l2': 'L2', 'r2': 'R2', 'l3': 'Left_Stick', 'r3': 'Right_Stick', 'touchpad': 'Square'}),
    'switch': ('Switch/Switch_', {
        'cross': 'B', 'circle': 'A', 'triangle': 'X', 'square': 'Y', 'l1': 'LB', 'r1': 'RB',
        'l2': 'LT', 'r2': 'RT', 'l3': 'Left_Stick', 'r3': 'Right_Stick', 'touchpad': 'Minus'}),
}


def write_png(path, width, height, rgba):
    """RGBA PNG with every row unfiltered (what buttons.read_png reads quickest)."""
    raw = b''.join(b'\0' + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    with open(path, 'wb') as w:
        w.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--game', required=True, type=Path)
    p.add_argument('--prompts', required=True, type=Path)
    p.add_argument('--out', type=Path, default=Path(__file__).resolve().parents[1] / 'assets' / 'button_icons')
    a = p.parse_args()
    archive = iga.Archive(a.game / buttons.ARCHIVE)
    width, height, rects = buttons.atlas(archive.read(buttons.FONTS[0]))[1:4]
    a.out.mkdir(parents=True, exist_ok=True)
    for style, (prefix, names) in ICONS.items():
        sheet = Image.new('RGBA', (width, height), (0, 0, 0, 0))
        for button, (x0, y0, x1, y1) in rects.items():
            icon = Image.open(a.prompts / f'{prefix}{names[button]}.png').convert('RGBA')
            icon = icon.crop(icon.getbbox())
            room_w, room_h = x1 - x0 - 2, y1 - y0 - 2
            scale = min(room_w / icon.width, room_h / icon.height)
            icon = icon.resize((max(1, round(icon.width * scale)), max(1, round(icon.height * scale))),
                               Image.LANCZOS)
            sheet.alpha_composite(icon, (x0 + (x1 - x0 - icon.width) // 2, y0 + (y1 - y0 - icon.height) // 2))
        write_png(a.out / f'{style}.png', width, height, sheet.tobytes())
        print(f'{a.out / style}.png')
    return 0


if __name__ == '__main__':
    sys.exit(main())
