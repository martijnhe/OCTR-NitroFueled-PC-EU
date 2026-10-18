"""Local CTR HUD shader-cache correction, controlled by the launcher's XML selection.

Only verified shaders are changed. Originals are backed up outside the cache;
disabling the XML restores them on the next launch. No game archives are edited.
"""
from pathlib import Path
import hashlib
import os
import struct

KEY = 'CTR-Centered-HUD-v4.xml/Centered 16:9 HUD v4 (32:9, 720p)'
CAMERA = 'CTR-SuperUltrawide-2560x720.xml/Experimental 32:9 (2560x720, PS4 Pro OFF)'
SIGNATURES = {
    'd4aaf527': 'd0050870723e20fc7ad53b765bdc6f74b662c134419e74503784787d87976116',
    '08948e32': 'a69c2785a4e066fd76262e1837364b5ecae2f8abe80bf7d45856081603775ae6',
}

def sha(data):
    return hashlib.sha256(data).hexdigest()

def centered_shader(data, factor=0.5):
    words = list(struct.unpack('<%dI' % (len(data)//4), data))
    if words[0] != 0x07230203:
        raise ValueError('Invalid shader header')
    instructions = []
    offset = 5
    while offset < len(words):
        length = words[offset] >> 16
        if not length or offset + length > len(words):
            raise ValueError('Invalid shader instruction')
        instructions.append((words[offset] & 65535, words[offset+1:offset+length]))
        offset += length
    position = next(a[0] for op,a in instructions if op == 71 and a[1:] == [11,0])
    float_type = next(a[0] for op,a in instructions if op == 22 and a[1:] == [32])
    vector_type = next(a[0] for op,a in instructions if op == 23 and a[1:] == [float_type,4])
    def instruction(op,*args):
        return [((len(args)+1)<<16)|op, *args]
    half, vector, x, scaled, result = range(words[3], words[3]+5)
    output = words[:5]
    output[3] += 5
    declared = False
    returns = 0
    for op,args in instructions:
        if op == 54 and not declared:
            output += instruction(43,float_type,half,struct.unpack('<I',struct.pack('<f',factor))[0])
            declared = True
        if op == 253:
            output += instruction(61,vector_type,vector,position)
            output += instruction(81,float_type,x,vector,0)
            output += instruction(133,float_type,scaled,x,half)
            output += instruction(82,vector_type,result,scaled,vector,0)
            output += instruction(62,position,result)
            returns += 1
        output += instruction(op,*args)
    if returns != 1:
        raise ValueError('Unexpected shader control flow')
    return struct.pack('<%dI' % len(output), *output)

def atomic_write(path, data):
    temporary = path.with_name(path.name+'.hud-tmp')
    temporary.write_bytes(data)
    os.replace(temporary,path)

def apply_hud_shaders(selected, out, title_id):
    if title_id != 'CUSA13795':
        return
    keys = {key for key,_,_ in selected}
    enabled = KEY in keys
    user = Path(os.environ.get('BB_GPU_USER_DIR',os.environ.get('BB_USER_DIR',str(Path(out).parent/'user'))))
    cache = user/'cache'/title_id
    backup = user/'hud-shader-backups-v4'
    if enabled:
        if CAMERA not in keys or os.environ.get('BB_NEO','0') != '0':
            raise RuntimeError('HUD v4 requires the 2560x720 camera patch and PS4 Pro OFF.')
        if any('centered' in key.lower() and key != KEY for key in keys):
            raise RuntimeError('Disable the older centered UI/HUD patches before enabling HUD v4.')
    operations = []
    for shader, signature in SIGNATURES.items():
        name = f'0x00000000{shader}_0.spv'
        path, original_path = cache/name, backup/name
        if not enabled and not original_path.exists():
            continue
        if not path.exists():
            if enabled:
                raise RuntimeError('HUD shader cache is missing. Disable HUD v4, launch once to build the cache, then enable HUD v4.')
            continue
        current = path.read_bytes()
        original = original_path.read_bytes() if original_path.exists() else current
        if sha(original) != signature:
            raise RuntimeError(f'HUD v4: unrecognized original shader {shader}; no files changed.')
        patched = centered_shader(original)
        if current not in (original,patched):
            raise RuntimeError(f'HUD v4: shader {shader} has another modification; no files changed.')
        operations.append((path,original_path,original,patched if enabled else original))
    # Validate every file before backing up or changing any.
    for path,original_path,original,target in operations:
        if not original_path.exists():
            backup.mkdir(parents=True,exist_ok=True)
            with original_path.open('xb') as stream:
                stream.write(original)
        if path.read_bytes() != target:
            atomic_write(path,target)
    if operations:
        print('HUD v4 shader correction: '+('enabled' if enabled else 'restored originals')+f' ({len(operations)} shaders)')

def known_ultrawide_shader(current, original):
    """Accept only byte-exact originals or either released aspect correction."""
    if current == original:
        return True
    factors = {0.5}
    for ratio in (3440/1440, 2560/1080):
        for height in (480, 720, 1080, 1440):
            factors.add((height*16/9)/(round(height*ratio/2)*2))
    return any(current == centered_shader(original, factor) for factor in factors)


def require_warm_cache(cache):
    """SPVs alone are insufficient: v0.3 regenerates them on a cold start."""
    files = [cache/'profile.bin'] + [cache/f'0x00000000{shader}_0.spv' for shader in SIGNATURES]
    if (any(not p.is_file() or p.stat().st_size == 0 for p in files)
            or not any(cache.glob('*.meta')) or not any(cache.glob('*.key'))):
        raise RuntimeError('UltraWide needs an existing HUD shader cache. Set Aspect / output to a 16:9 option, '
                           'launch to the main menu once and close the game normally, then pick the ultrawide option again. '
                           'No shader files were changed.')

