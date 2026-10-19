"""Three-way merge of an IGZ file (Alchemy, version 10): the US original, a US edit of it and the
EU original. The EU file differs from the US one only in some section-0 records (TDEP: the
dependencies); the edit changes other records and the data section. The merge takes each
section-0 record from whichever side changed it (both: refused) and the edit's data section."""
import struct

def sections(d):
    return [struct.unpack_from('<4I', d, 0x14 + 16 * i) for i in range(2)]  # (flags, offset, size, align)

def records(d):
    _, off, size, _ = sections(d)[0]
    out, p = [], off
    while p < off + size:
        magic, count, length, start = struct.unpack_from('<4sIII', d, p)
        if not length: raise ValueError('bad IGZ record')
        out.append((magic, d[p:p + length])); p += length
    return out

def merge(us, edit, eu):
    for d in (us, edit, eu):
        if d[:4] != b'\x01ZGI' or struct.unpack_from('<I', d, 4)[0] != 10: raise ValueError('not an IGZ v10 file')
    if us[0x40:0x800] != edit[0x40:0x800] or us[0x40:0x800] != eu[0x40:0x800]: raise ValueError('headers differ')
    ru, re_, rx = records(us), records(edit), records(eu)
    if [m for m, _ in ru] != [m for m, _ in re_] or [m for m, _ in ru] != [m for m, _ in rx]: raise ValueError('record lists differ')
    merged = []
    for (m, a), (_, b), (_, c) in zip(ru, re_, rx):
        if b != a and c != a: raise ValueError('both sides change ' + m.decode())
        merged.append(c if c != a else b)
    # EU's data section must be the US one: the edit's then applies unchanged.
    (_, uo, us1off, _), (_, _, _, _) = sections(us)[0], sections(us)[1]
    s_us, s_eu, s_ed = sections(us)[1], sections(eu)[1], sections(edit)[1]
    if us[s_us[1]:s_us[1] + s_us[2]] != eu[s_eu[1]:s_eu[1] + s_eu[2]]: raise ValueError('data sections differ')
    sec0 = b''.join(merged)
    off0 = sections(edit)[0][1]
    pad = (s_ed[1] - (off0 + sections(edit)[0][2]))  # the edit's own gap before the data section
    off1 = off0 + len(sec0) + pad
    data = edit[s_ed[1]:s_ed[1] + s_ed[2]]
    head = bytearray(edit[:off0])
    struct.pack_into('<I', head, 0x14 + 8, len(sec0))
    struct.pack_into('<I', head, 0x14 + 16 + 4, off1)
    out = bytes(head) + sec0 + b'\0' * pad + data + edit[s_ed[1] + s_ed[2]:]
    # Check: the result parses, with every record from the side it should come from.
    if [r for _, r in records(out)] != merged: raise ValueError('merge check failed')
    return out
