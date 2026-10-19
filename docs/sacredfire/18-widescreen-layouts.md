# Corrected widescreen layouts (21:9 and 32:9)

Settings -> Display -> **Aspect ratio** offers 16:9 (the default), 21:9 and 32:9, and then the
screen size: 3440x1440 or 2560x1080 for 21:9; 5120x1440, 3840x1080, 2560x720 or 7680x2160 for
32:9. Both launchers write `widescreen_layout` and `widescreen_output` to bbport.ini. The layouts
and their data are by tcosi (scripts/widescreen_data, nitro_format.py, nitro_components.py,
archive_reader.py); scripts/widescreen.py applies them at each launch, from run.py and run.sh.

A layout corrects what the in-game ultrawide (the monitor's size, `ultrawide=1`) leaves at 16:9:
menu backgrounds, masks and clipping, text and HUD positions, Cup Selection, Hints, Progression
and the boost gauge. Choosing one turns that ultrawide off, since the layout sets the size itself.

## What a launch with a layout does

- **Assets**: 224 changes in 13 archives per asset set (the two 21:9 sizes have separate sets).
  They are rebuilt from the player's own archives (byte deltas, and copy deltas for the resized Cup
  Selection assets), each checked against its original and result SHA-256, and stored as archive
  patches in `out/widescreen/` (about 9 MB), made once per set. The game reads them through
  `BB_ARCHIVE_PATCHES` (src/runtime_file.c); the archives are never written. The online playlist's
  patch of update2.pak is kept inside the layout's patch of that archive.
- **Executable**: the variant for the internal resolution (render_res 480 to 2160), 15 writes
  located by signature, written to `out/widescreen/patches/Widescreen.xml` and read with the
  bundled patches. scripts/patches.py reports patches that write the same bytes.
- **HUD**: two vertex shaders (0xd4aaf527 and 0x08948e32) get their output x scaled (16:9 over
  the screen's aspect) when their Vulkan module is made, `BB_HUD_SCALE`
  (gpu/.../renderer_vulkan/bb_hud_scale.h). The shader cache keeps the shaders as they are, so
  16:9 and every layout share it. The transform is tcosi's `position_scale`; the C++ one gives
  the same bytes for all 24 variants (checked against the data's SHA-256).
- **Render settings**: scripts/patches.py and the render path read `out/widescreen/bbport.ini`,
  the player's settings with the ones the layouts were made for: render_res 1080 on the base PS4
  path (the variant sets the size), no FSR or DLSS (`BB_TEMPORAL=off`), ultrawide off. The
  player's bbport.ini, saves and shader cache are the usual ones.

## The European edition

CUSA14876 1.21 gets the same layouts through GetLostIn's port (their commit 403445c,
GPL-2.0), in `scripts/widescreen_data/eu`:

- `ultrawide_eu.json`: the executable runs the same code with two patch sites 16 bytes
  earlier and three constants 64 bytes earlier; `port_meta` moves each line, re-aims its
  RIP-relative and branch operands and gives it the EU bytes as its signature. All 12 variants
  apply their 15 writes to the EU 1.21 executable.
- `assets.json`: the 224 asset changes of each set ported per file to the EU archives (their UI
  files are the USA ones with other strings), checked by SHA-256 like the USA ones. Their port
  shipped the resized Cup Selection file whole (3 entries); here it is a copy delta from the
  player's EU original (`payloads/`), so no game asset is distributed. All 672 read back with
  their expected SHA-256.

## When the game starts at 16:9 instead

The launch says why (`Widescreen: ...`) and goes on at 16:9 when:

- the game is not the 1.21 update (USA or EU) the data was verified with (the executable's
  SHA-256; the image's for the EU edition);
- online is on: online with a layout has not been tested yet (`ONLINE_TESTED` in widescreen.py);
- an asset is not the expected one (a mod changed it), or the data does not validate.

## Checks

`python -m unittest discover -s tests -p test_widescreen.py`: variant selection for every size
and internal resolution, copy deltas, the HUD list, and archive patches stacked on another patch
(on a synthetic archive). With the game, all 672 rebuilt assets of the three sets read back
through the patches with their expected SHA-256, and the 12 executable variants apply with 15
writes each without overlapping the common patches (60 FPS, motion blur).
