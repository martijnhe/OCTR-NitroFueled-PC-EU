#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Lists and extracts files of the game's Alchemy archives (archives/*.pak, IGA version 13), for
research on your own dump (layout in scripts/iga.py).

  iga_extract.py ARCHIVE list SUBSTRING     the files whose path contains SUBSTRING (and sizes)
  iga_extract.py ARCHIVE PATH OUTPUT        extract one file
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'scripts'))
import iga  # noqa: E402

if __name__ == '__main__':
    archive = iga.Archive(sys.argv[1])
    if sys.argv[2] == 'list':
        for key, (_offset, length, _block, name) in archive.files.items():
            if sys.argv[3].lower() in key:
                print(name, length)
    else:
        data = archive.read(sys.argv[2])
        Path(sys.argv[3]).write_bytes(data)
        print(len(data))
