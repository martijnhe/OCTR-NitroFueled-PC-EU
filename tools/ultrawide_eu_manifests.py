"""Ports tcosi's ultrawide asset deltas (made for CUSA13795) to the European edition (CUSA14876).

The EU copies of the patched UI assets are the US ones with a few hundred bytes more (other
strings), so each run of old->new bytes is found in the EU file by its surrounding bytes in the US
original (a unique match of 16..256 bytes around it) and its old bytes are checked there. Whole-file
replacements are turned into runs first (US original against the US replacement). Every EU result is
checked again from scratch. Writes the EU manifests next to the US ones.
Replacements that add objects are merged per IGZ record (igz_merge.py).
usage: ultrawide_eu_manifests.py <port scripts> <US game> <EU game>"""
import sys, json, hashlib, zlib, base64, copy
from pathlib import Path
scripts = Path(sys.argv[1]); sys.path.insert(0, str(scripts))
from ultrawide_21x9_archive import Archive
sys.path.insert(0, str(Path(__file__).resolve().parent))
from igz_merge import merge as igz_merge
US, EU = Path(sys.argv[2]) / 'archives', Path(sys.argv[3]) / 'archives'
JOBS = [('ultrawide_21x9_data/manifest-3440.json', 'ultrawide_21x9_data/manifest-eu-3440.json'),
        ('ultrawide_21x9_data/manifest-2560.json', 'ultrawide_21x9_data/manifest-eu-2560.json'),
        ('ultrawide_32x9_data/manifest.json', 'ultrawide_32x9_data/manifest-eu.json')]
sha = lambda b: hashlib.sha256(b).hexdigest()
def file_sha(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for c in iter(lambda: f.read(1 << 24), b''): h.update(c)
    return h.hexdigest()
opened = {}
def archive(root, name):
    key = (str(root), name)
    if key not in opened:
        a = Archive(root / name); opened[key] = (a, {f['path']: f for f in a.files})
    return opened[key]
def patched(data, entry):
    if 'replacement_zlib' in entry:
        return zlib.decompress(base64.b64decode(entry['replacement_zlib']))
    out = bytearray(data)
    for off, old, new in entry['runs']:
        old, new = bytes.fromhex(old), bytes.fromhex(new)
        assert out[off:off + len(old)] == old
        out[off:off + len(new)] = new
    return bytes(out)
def diff_runs(a, b):
    assert len(a) == len(b)
    runs, i = [], 0
    while i < len(a):
        if a[i] != b[i]:
            j = i
            while j < len(a) and a[j] != b[j]: j += 1
            runs.append([i, a[i:j].hex(), b[i:j].hex()]); i = j
        else: i += 1
    return runs
def map_offset(us, eu, off, n):
    for ctx in (16, 32, 64, 128, 256):
        lo, hi = max(0, off - ctx), min(len(us), off + n + ctx)
        needle = us[lo:hi]
        first = eu.find(needle)
        if first < 0: continue
        if eu.find(needle, first + 1) < 0: return first + (off - lo)
    # The bytes after the run alone (the run may sit right after an EU-only change).
    for ctx in (32, 64, 128):
        needle = us[off:off + n + ctx]
        first = eu.find(needle)
        if first >= 0 and eu.find(needle, first + 1) < 0: return first
    return None
whole = {}
problems = []
for src, dst in JOBS:
    m = json.loads((scripts / src).read_text()); out = copy.deepcopy(m)
    out['title_id'] = 'CUSA14876'; out['ported_from'] = 'CUSA13795'
    for a_us, a_eu in zip(m['archives'], out['archives']):
        name = a_us['archive']
        if name not in whole: whole[name] = (file_sha(EU / name), (EU / name).stat().st_size)
        a_eu['original_sha256'], a_eu['original_size'] = whole[name]
        us_ar, us_by = archive(US, name); eu_ar, eu_by = archive(EU, name)
        for f_us, f_eu in zip(a_us['files'], a_eu['files']):
            us_data = us_ar.extract(us_by[f_us['path']])
            assert sha(us_data) == f_us['original_sha256'], ('US original differs', name, f_us['path'])
            us_new = patched(us_data, f_us)
            assert sha(us_new) == f_us['patched_sha256']
            e = eu_by[f_us['path']]; eu_data = eu_ar.extract(e)
            runs = diff_runs(us_data, us_new) if len(us_data) == len(us_new) else None
            if runs is None:
                # A replacement that adds objects (21:9 Octane_CupSelect): merged into the EU file.
                try:
                    eu_new = igz_merge(us_data, us_new, eu_data)
                except ValueError as error:
                    problems.append([src, name, f_us['path'], 'replacement: ' + str(error)]); continue
                f_eu.update(index=e['index'], size=len(eu_new), original_sha256=sha(eu_data), patched_sha256=sha(eu_new),
                            runs=[], replacement_zlib=base64.b64encode(zlib.compress(eu_new, 9)).decode())
                continue
            eu_runs, ok = [], True
            for off, old, new in runs:
                n = len(old) // 2
                at = us_data == eu_data and off or map_offset(us_data, eu_data, off, n)
                if at is None or eu_data[at:at + n].hex() != old:
                    ok = False; problems.append([src, name, f_us['path'], 'run at %d not found' % off]); break
                eu_runs.append([at, old, new])
            if not ok: continue
            eu_new = bytearray(eu_data)
            for at, old, new in eu_runs: eu_new[at:at + len(new) // 2] = bytes.fromhex(new)
            f_eu.pop('replacement_zlib', None)
            f_eu.update(index=e['index'], size=len(eu_data), original_sha256=sha(eu_data), patched_sha256=sha(bytes(eu_new)),
                        runs=eu_runs)
    (scripts / dst).write_text(json.dumps(out, separators=(',', ':')))
    print('wrote', dst)
print('problems:', len(problems))
for p in problems[:40]: print(' ', p)
