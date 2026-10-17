#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Button icons in another controller's style (Xbox, Steam Deck, Switch) in place of the PS4's.
The game draws its button icons as font characters (U+E000-E012, GuiSystemData's button
characters) from a second texture in the fonts of the update (fonts/geekspeak.igz,
geekspeak_outline.igz and zoinks_outline.igz in archives/update.pak): a 512x263 RGBA8 image,
stored in 8x8 tiles (the pixels of a tile in Morton order: x0 y0 x1 y1 x2 y2), blue first.
This writes, from the player's own update.pak, a byte overlay (BB_FILE_OVERLAY,
src/runtime_file.c) in which each icon's rectangle holds the style's icon from
assets/button_icons/STYLE.png (tools/button_icons.py: Xelu's Free Controller Prompts, Those
Awesome Guys, CC0). The icons stand for the buttons by position: the bottom face button (the
game's confirm) is A on Xbox and the Deck, B on Switch.

  buttons.py --game GAME --out FILE --style {playstation,xbox,deck,switch}

Overlay format: as scripts/startscreen.py's. playstation: no file.
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import iga  # noqa: E402

ARCHIVE = 'archives/update.pak'
FONTS = ('fonts/geekspeak.igz', 'fonts/geekspeak_outline.igz', 'fonts/zoinks_outline.igz')
STYLES = ('playstation', 'xbox', 'deck', 'switch')
ICONS = Path(__file__).resolve().parents[1] / 'assets' / 'button_icons'
# The font characters of the PS4 icons (in the button texture).
CODES = {0xe000: 'cross', 0xe001: 'circle', 0xe002: 'triangle', 0xe003: 'square', 0xe004: 'l1',
         0xe006: 'r1', 0xe007: 'l2', 0xe008: 'r2', 0xe009: 'l3', 0xe00a: 'r3', 0xe012: 'touchpad'}
IMAGE_SIZE = 0x18      # igImage2: u16 width, u16 height
IMAGE_BYTES = 0x38     # igImage2: u32 size of the pixels
IMAGE_DATA = 0x40      # igImage2: u32 memory reference (section above the objects << 27 | offset)
METRIC_CODE = 0x0c     # igCharMetrics: u32 character
METRIC_UV = 0x34       # igCharMetrics: float u0, v0, u1, v1
METRIC_PAGE = 0x44     # igCharMetrics: u32 0 in the button list


def atlas(data):
    """(file offset of the button texture's pixels, width, height, {button: (x0, y0, x1, y1)}
    inclusive, stored rows)."""
    objects = iga.Objects(data)
    images = [o for o in objects.objects if objects.type_of(o) == 'igImage2']
    if len(images) != 2:
        raise ValueError('a font without a button texture')
    at = objects.base + images[1]
    width, height = struct.unpack_from('<HH', data, at + IMAGE_SIZE)
    size, reference = struct.unpack_from('<I', data, at + IMAGE_BYTES)[0], struct.unpack_from('<I', data, at + IMAGE_DATA)[0]
    rows = (height + 7) // 8 * 8
    if width % 8 or size != width * rows * 4:
        raise ValueError(f'a button texture of {width}x{height} in {size} bytes')
    sections = [struct.unpack_from('<4I', data, 0x10 + 16 * k)[2:] for k in range(4)]
    sections = [(offset, length) for offset, length in sections if offset and length]
    section, offset = sections[1 + (reference >> 27)], reference & 0x7ffffff
    if offset + size > section[1]:
        raise ValueError('the button texture is out of its section')
    rects = {}
    for o in objects.objects:
        if objects.type_of(o) != 'igCharMetrics':
            continue
        code = struct.unpack_from('<I', data, objects.base + o + METRIC_CODE)[0]
        if code in CODES and struct.unpack_from('<I', data, objects.base + o + METRIC_PAGE)[0] == 0:
            u0, v0, u1, v1 = struct.unpack_from('<4f', data, objects.base + o + METRIC_UV)
            rects[CODES[code]] = (round(u0 * width), round(v0 * height),
                                  min(round(u1 * width), width - 1), min(round(v1 * height), height - 1))
    if len(rects) != len(CODES):
        raise ValueError(f'a font without the PS4 button icons ({len(rects)} of {len(CODES)})')
    return section[0] + offset, width, height, rects, rows


def read_png(path):
    """(width, height, RGBA bytes) of an 8-bit RGBA PNG, not interlaced."""
    data = Path(path).read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'{path}: not a PNG')
    at, idat, header = 8, b'', None
    while at < len(data):
        length, kind = struct.unpack_from('>I4s', data, at)
        body = data[at + 8:at + 8 + length]
        if kind == b'IHDR':
            header = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat += body
        at += 12 + length
    width, height, depth, color, _c, _f, interlace = header
    if (depth, color, interlace) != (8, 6, 0):
        raise ValueError(f'{path}: not 8-bit RGBA')
    raw, stride = zlib.decompress(idat), width * 4
    out, previous = bytearray(), bytearray(stride)
    for y in range(height):
        kind, row = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            left, up = row[i - 4] if i >= 4 else 0, previous[i]
            corner = previous[i - 4] if i >= 4 else 0
            if kind == 1:
                row[i] = (row[i] + left) & 0xff
            elif kind == 2:
                row[i] = (row[i] + up) & 0xff
            elif kind == 3:
                row[i] = (row[i] + (left + up) // 2) & 0xff
            elif kind == 4:
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                row[i] = (row[i] + (left if pa <= pb and pa <= pc else up if pb <= pc else corner)) & 0xff
        out += row
        previous = row
    return width, height, bytes(out)


def tiled_index(width):
    """For each pixel (row by row), its index in the tiled texture."""
    morton = [0] * 64
    for y in range(8):
        for x in range(8):
            morton[y * 8 + x] = ((x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2
                                 | (y & 4) << 3)
    tiles = width // 8
    return lambda x, y: ((y // 8) * tiles + x // 8) * 64 + morton[(y % 8) * 8 + x % 8]


def restyle(data, sheet):
    """The font with its button texture's icon rectangles taken from the sheet (RGBA)."""
    start, width, height, rects, _rows = atlas(data)
    sheet_width, sheet_height, pixels = sheet
    if (sheet_width, sheet_height) != (width, height):
        raise ValueError(f'the icon sheet is {sheet_width}x{sheet_height}, the texture {width}x{height}')
    out, index = bytearray(data), tiled_index(width)
    for x0, y0, x1, y1 in rects.values():
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                r, g, b, a = pixels[(y * width + x) * 4:(y * width + x) * 4 + 4]
                at = start + index(x, y) * 4
                out[at:at + 4] = bytes((b, g, r, a))
    return bytes(out)


def overlay(game, style):
    archive = iga.Archive(Path(game) / ARCHIVE)
    sheet = read_png(ICONS / f'{style}.png')
    out = []
    for name in FONTS:
        # The icons compress smaller than the PS4's: the pieces take less than their room.
        out += iga.replacement(archive, name, restyle(archive.read(name), sheet), shrink=True)
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--game', required=True, type=Path)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--style', required=True, choices=STYLES)
    a = p.parse_args()
    if a.style == 'playstation':
        a.out.unlink(missing_ok=True)
        return 0
    try:
        records = overlay(a.game, a.style)
    except (OSError, ValueError, KeyError) as e:
        print(f'Button icons: {e}; PS4 icons', file=sys.stderr)
        a.out.unlink(missing_ok=True)
        return 1
    with open(a.out, 'wb') as w:
        w.write(b'BBOVL1\n')
        path = ('/app0/' + ARCHIVE).encode()
        for offset, data in records:
            w.write(struct.pack('<H', len(path)) + path + struct.pack('<QI', offset, len(data)) + data)
    print(f'Button icons: {a.style} ({sum(len(d) for _, d in records)} bytes over {ARCHIVE})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
