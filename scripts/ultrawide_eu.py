"""tcosi's ultrawide (made for CUSA13795) on the European edition, CUSA14876 1.21 (v0.4).

Executable: the EU 1.21 runs the same code; two patch sites sit 16 bytes earlier and three
constants the patches read 64 bytes earlier (ultrawide_eu.json, worked out once against both
executables). port_meta() moves each line, re-aims its RIP-relative and branch operands, and gives
it the EU bytes as its signature, so patches.py still checks every site before writing.

Assets: the EU UI files are the US ones with other strings; the deltas were ported per file
(manifest-eu-*.json, made by tools/ultrawide_eu_manifests.py from both games' archives), and are
checked byte for byte like the US ones. The HUD shaders are the same; the cache is the EU title's.
"""
import copy
import json
from pathlib import Path

TITLE = 'CUSA14876'
DATA = json.loads((Path(__file__).resolve().parent / 'ultrawide_eu.json').read_text(encoding='utf-8'))
LINE_DELTA = {int(k, 0): v for k, v in DATA['line_delta'].items()}
TARGET_DELTA = {int(k, 0): v for k, v in DATA['target_delta'].items()}
OPERANDS = {int(k, 0): v for k, v in DATA['operands'].items()}
SIGNATURES = {int(k, 0): v for k, v in DATA['signatures'].items()}


def is_eu(title):
    return title == TITLE


def port_meta(meta):
    """A copy of a US ultrawide <Metadata> for the EU executable."""
    meta = copy.deepcopy(meta)
    lines = [(int(l.get('Address'), 0), l) for l in meta.iter('Line')]
    spans = [(a, a + len(l.get('Value')) // 2) for a, l in lines]
    def moved(target):
        for start, end in spans:
            if start <= target < end:
                return target + LINE_DELTA.get(start, 0)
        return target + TARGET_DELTA.get(target, 0)
    for a, line in lines:
        if a not in OPERANDS:
            raise RuntimeError(f'Ultrawide (EU): no port data for the patch at {a:#x}')
        blob = bytearray.fromhex(line.get('Value'))
        site = a + LINE_DELTA.get(a, 0)
        for field, size, end in OPERANDS[a]:
            disp = int.from_bytes(blob[field:field + size], 'little', signed=True)
            new = moved(a + end + disp) - (site + end)
            limit = 1 << (8 * size - 1)
            if not -limit <= new < limit:
                raise RuntimeError(f'Ultrawide (EU): operand at {a:#x}+{field} out of range')
            blob[field:field + size] = new.to_bytes(size, 'little', signed=True)
        line.set('Address', f'{site:#x}')
        line.set('Value', blob.hex())
        line.set('Signature', SIGNATURES[a])
    return meta


def manifest_name(us_name):
    """manifest-3440.json -> manifest-eu-3440.json; manifest.json -> manifest-eu.json."""
    stem = us_name[:-len('.json')]
    return stem.replace('manifest', 'manifest-eu', 1) + '.json'
