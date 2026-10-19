#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""UltraWide v1.0 by tcosi for run.sh: the Linux counterpart of run.py's uw_prepare / uw_refresh /
uw_shaders (scripts/ultrawide_dispatch.py), as a command line.

  ultrawide_linux.py prepare --port P --data D --game G --config C --env-file F
      Builds (once) the patched game view for bbport.ini aspect_ratio=21:9|32:9 and writes the
      environment run.sh exports, NUL-separated KEY=VALUE: BB_UW_ENABLED, BB_UW_GAME (the folder
      to run), BB_UW_RENDER_CONFIG (the settings patches.py compiles from), BB_CONFIG,
      BB_PATCHES_CONFIG, BB_PATCHES_DIR, BB_NEO, BB_DMEM_MB (above 1440p internal).
  ultrawide_linux.py shaders --port P --data D --game G --config C --user U --out O
      After prepare.py made out/eboot.elf: checks the executable signatures and writes (or, at 16:9,
      removes) the centered HUD shaders in the shader cache.

--config is always the player's own bbport.ini (not the copy prepare hands back). The legacy
bbport.ini ultrawide=1 output patch is left out of the compiled settings while tcosi's is on: the
two patch the same output size.
"""
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import sys

TITLES = ('CUSA13795', 'CUSA14876')


def strip_legacy(path):
    lines = Path(path).read_text(encoding='utf-8').splitlines()
    kept = [l for l in lines if l.partition('=')[0].strip() not in ('ultrawide', 'ultrawide_size')]
    if kept != lines:
        Path(path).write_text(''.join(l + '\n' for l in kept), encoding='utf-8')


FORCED = ('temporal', 'upscaler', 'patch_pro_1080p')  # what the implementations override


def keep_menu_changes(data, config):
    """The game runs with the implementation's copy of bbport.ini (out/ultrawide-*/bbport.ini) and
    the in-game menu saves there; what it changed since goes back to the player's own, but for the
    settings the copy forces."""
    config = Path(config)
    for copy in Path(data, 'out').glob('ultrawide-*/bbport.ini'):
        if not config.is_file() or copy.stat().st_mtime_ns <= config.stat().st_mtime_ns:
            continue
        changed = {}
        for line in copy.read_text(encoding='utf-8').splitlines():
            key, sep, value = line.partition('=')
            if sep and key.strip() not in FORCED:
                changed[key.strip()] = value
        lines = []
        for line in config.read_text(encoding='utf-8').splitlines():
            key, sep, _ = line.partition('=')
            lines.append(f'{key.strip()}={changed.pop(key.strip())}' if sep and key.strip() in changed else line)
        lines += [f'{k}={v}' for k, v in changed.items()]
        config.write_text(''.join(l + '\n' for l in lines), encoding='utf-8')


def prepare(args, env):
    if args.command == 'prepare':
        keep_menu_changes(args.data, args.config)
    sys.path.insert(0, str(Path(args.port) / 'scripts'))
    from game_check import sfo_info
    import ultrawide_dispatch
    title = sfo_info(Path(args.game)).get('TITLE_ID') or ''
    if title not in TITLES:
        if ultrawide_dispatch.aspect_setting(Path(args.config)) in ('21:9', '32:9'):
            print(f'Ultrawide: made for {" and ".join(TITLES)}; {title} runs at 16:9', file=sys.stderr)
        return None
    # The helpers report progress on stdout; run.sh reads only the environment file.
    with contextlib.redirect_stdout(sys.stderr):
        mode = ultrawide_dispatch.prepare(Path(args.port), Path(args.data), env, Path(args.game), str(args.config))
    if mode.enabled:
        strip_legacy(mode.render_config)
    return mode


def shaders(mode, user_dir, out):
    """The implementations' shaders(), with this machine's own HUD shaders as the originals.

    tcosi's package checks the cache against the SPIR-V the Windows build compiles; this build (and
    another GPU) compiles the same game shader to other bytes. The correction itself only scales
    gl_Position.x, so it is applied to the cached shader, whose original is kept in
    user/ultrawide-hud-originals/<title> and put back at 16:9. state.json holds the hash of each
    file written here: a cache file with another hash is a fresh compile, the new original.
    """
    implementation = mode.implementation
    super_wide = implementation.__name__ == 'ultrawide_32x9'
    label = 'Super UltraWide' if super_wide else '21:9 UltraWide'
    if mode.enabled:
        from patches import compile_external, eboot_segments
        elf = (out / 'eboot.elf').read_bytes()
        writes = compile_external(mode.selected, eboot_segments(elf), elf)
        if len(writes) != sum(len(list(m.iter('Line'))) for _, _, m in mode.selected):
            raise RuntimeError(f'{label} executable signatures do not match; launch stopped.')
    cache = user_dir / 'cache' / implementation.TITLE
    originals = user_dir / 'ultrawide-hud-originals' / implementation.TITLE
    if mode.enabled:
        implementation.require_warm_cache(cache)
    factor = 0.5 if super_wide or not mode.enabled else (mode.height * 16 / 9) / mode.width
    state_file = originals / 'state.json'
    state = json.loads(state_file.read_text(encoding='utf-8')) if state_file.is_file() else {}
    operations = []
    for shader in implementation.SIGNATURES:
        name = f'0x00000000{shader}_0.spv'
        path, kept = cache / name, originals / name
        if not path.is_file():
            continue
        current = path.read_bytes()
        if state.get(name) == hashlib.sha256(current).hexdigest():
            if not kept.is_file():
                raise RuntimeError(f'the original of HUD shader {name} is missing; delete it from {cache} '
                                   'to have the game compile it again.')
            original = kept.read_bytes()
        else:
            original = current
        try:
            corrected = implementation.centered_shader(original, factor)
        except (ValueError, StopIteration):
            raise RuntimeError(f'HUD shader {name} is not the expected one; launch stopped.') from None
        operations.append((name, path, kept, original, corrected if mode.enabled else original))
    # Every shader checked before any file changes.
    for name, path, kept, original, desired in operations:
        if mode.enabled:
            implementation.atomic(kept, original)
            state[name] = hashlib.sha256(desired).hexdigest()
        else:
            state.pop(name, None)
        if path.read_bytes() != desired:
            implementation.atomic(path, desired)
    if operations:
        implementation.atomic(state_file, json.dumps(state, indent=2).encode('utf-8'))
        what = f'centered (x{factor:.4f})' if mode.enabled else 'originals'
        print(f'{label}: HUD shaders {what} ({len(operations)})')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('command', choices=('prepare', 'shaders'))
    for name in ('--port', '--data', '--game', '--config'):
        parser.add_argument(name, required=True)
    parser.add_argument('--env-file')
    parser.add_argument('--user')
    parser.add_argument('--out')
    args = parser.parse_args()
    env = dict(os.environ)
    # run.sh exported what prepare handed back; the shaders step prepares from the player's own.
    work = (Path(args.data) / 'out').resolve()
    for key in ('BB_CONFIG', 'BB_PATCHES_CONFIG', 'BB_PATCHES_DIR'):
        if key in env and Path(env[key]).resolve().is_relative_to(work) \
                and Path(env[key]).resolve().relative_to(work).parts[0].startswith('ultrawide-'):
            del env[key]
    try:
        mode = prepare(args, env)
        if args.command == 'prepare':
            values = {'BB_UW_ENABLED': '1' if mode and mode.enabled else '0'}
            if mode and mode.enabled:
                values.update(BB_UW_GAME=str(mode.game), BB_UW_RENDER_CONFIG=mode.render_config)
                values.update({k: env[k] for k in ('BB_CONFIG', 'BB_PATCHES_CONFIG', 'BB_PATCHES_DIR', 'BB_NEO', 'BB_DMEM_MB')
                               if k in env})
            Path(args.env_file).write_text(''.join(f'{k}={v}\0' for k, v in values.items()), encoding='utf-8')
        elif mode is not None:
            with contextlib.redirect_stdout(sys.stderr):
                shaders(mode, Path(args.user), Path(args.out))
    except RuntimeError as error:
        print(f'Ultrawide: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
