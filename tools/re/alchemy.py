#!/usr/bin/env python3
"""Reverse-engineering helpers over a prepared image (an out/ directory from scripts/prepare.py:
boot.bin holds the loaded image and its relocations). Addresses are image offsets (a patch's
Address is offset + 0x400000). Needs capstone and numpy.

    alchemy.py OUT fields NAME...   Alchemy field tables containing NAME, with field offsets
    alchemy.py OUT str TEXT...      where strings are, and what references them
    alchemy.py OUT dis ADDR [LEN]   disassembly (RIP-relative targets and strings annotated)
    alchemy.py OUT refs ADDR...     code (lea/mov rip+X, call) and data (relocated pointer) references
"""
import struct
import sys

import numpy as np
from capstone import CS_ARCH_X86, CS_MODE_64, Cs


def load_boot(path):
    b = open(path, 'rb').read()
    _magic, size, _entry, nl, nr, nn, _ = struct.unpack_from('<8sQQQQQQ', b, 0)
    p = 56 + nl * 24 + nn * 128
    relocs = np.frombuffer(b[p:p + nr * 32], dtype=np.int64).reshape(nr, 4)
    p += nr * 32
    return b[p:p + size], relocs


class Image:
    def __init__(self, out, code_end=0x1d00000):
        self.img, r = load_boot(out + '/boot.bin')
        base = r[:, 1] == 0
        self.rel = dict(zip(r[base, 0].tolist(), r[base, 2].tolist()))
        self.inv = {}
        for t, a in self.rel.items():
            self.inv.setdefault(a, []).append(t)
        self.code_end = min(code_end, len(self.img))
        a = np.frombuffer(self.img[:self.code_end], dtype=np.uint8)
        n = len(a) - 8
        d = a.astype(np.int64)
        def disp(at):
            v = d[at:n + at] | (d[at + 1:n + at + 1] << 8) | (d[at + 2:n + at + 2] << 16) | (d[at + 3:n + at + 3] << 24)
            return np.where(v >= 2**31, v - 2**32, v)
        self.rip7 = np.arange(n) + 7 + disp(3)            # REX op modrm(rip) disp32
        self.rip7_ok = ((a[2:n + 2] & 0xC7) == 0x05) & ((a[:n] & 0xF0) == 0x40)
        self.call = np.arange(n) + 5 + disp(1)
        self.call_ok = a[:n] == 0xE8
        self.md = Cs(CS_ARCH_X86, CS_MODE_64)

    def s(self, a):
        try:
            t = self.img[a:self.img.index(b'\0', a)].decode()
            return t if t.isprintable() else '?'
        except (ValueError, UnicodeDecodeError):
            return '?'

    def find_str(self, text):
        b = b'\0' + text.encode() + b'\0'
        out, i = [], self.img.find(b)
        while i != -1:
            out.append(i + 1)
            i = self.img.find(b, i + 1)
        return out

    def code_refs(self, t):
        return np.nonzero((self.rip7 == t) & self.rip7_ok)[0].tolist()

    def callers(self, t):
        return np.nonzero((self.call == t) & self.call_ok)[0].tolist()

    def data_refs(self, t):
        return self.inv.get(t, [])

    def ptr(self, a):
        return self.rel.get(a, struct.unpack_from('<Q', self.img, a)[0])

    def dis(self, a, n=0x80, show=True):
        lines = []
        for i in self.md.disasm(self.img[a:a + n], a):
            note = ''
            if 'rip' in i.op_str:
                try:
                    part = i.op_str.split('rip ')[1].split(']')[0]
                    t = i.address + i.size + int(part[2:], 16) * (1 if part[0] == '+' else -1)
                    st = self.s(t)
                    note = f'   ; {t:#x}' + (f' "{st[:60]}"' if st != '?' and len(st) > 2 else '')
                except (IndexError, ValueError):
                    pass
            lines.append(f'{i.address:08x}  {i.mnemonic:6} {i.op_str}{note}')
        if show:
            print('\n'.join(lines))
        return lines

    def func_start(self, a):
        x = a
        while x > 0:
            if self.img[x] == 0x55 and self.img[x + 1:x + 4] == b'\x48\x89\xe5' and self.img[x - 1] in (0xcc, 0xc3, 0x90, 0x00):
                return x
            x -= 1
        return None

    def field_tables(self, name):
        """[(names_table, [(offset or None, field)], registration_site, storage_table)] for tables
        holding the field NAME (Alchemy reflection: names, per-field metadata storage and u16
        offsets are passed together to the class's field registration)."""
        found, seen = [], set()
        for a in self.find_str(name):
            for slot in self.data_refs(a):
                x = slot
                while self.s(self.rel.get(x - 8, 0)).startswith('_'):
                    x -= 8
                if x in seen:
                    continue
                seen.add(x)
                names, y = [], x
                while self.s(self.rel.get(y, 0)).startswith('_'):
                    names.append(self.s(self.rel[y]))
                    y += 8
                offsets, site, storage = [None] * len(names), None, None
                for ref in self.code_refs(x):
                    for i in self.md.disasm(self.img[ref:ref + 0x20], ref):
                        if 'rip' not in i.op_str or i.mnemonic != 'lea':
                            continue
                        part = i.op_str.split('rip ')[1].rstrip(']')
                        t = i.address + i.size + int(part[2:], 16) * (1 if part[0] == '+' else -1)
                        if i.op_str.startswith('rdx'):
                            storage = t
                        elif i.op_str.startswith('rcx'):
                            offsets = list(struct.unpack_from(f'<{len(names)}H', self.img, t))
                            site = ref
                found.append((x, list(zip(offsets, names)), site, storage))
        return found


def main(argv):
    im = Image(argv[0])
    cmd, args = argv[1], argv[2:]
    if cmd == 'fields':
        for name in args:
            for table, fields, site, storage in im.field_tables(name):
                print(f'== {name}: names {table:#x}, storage {storage and hex(storage)}, registered at {site and hex(site)}')
                for off, field in fields:
                    print(f'   {off:#06x} {field}' if off is not None else f'   ?      {field}')
    elif cmd == 'str':
        for text in args:
            for a in im.find_str(text):
                print(text, hex(a), 'code', [hex(x) for x in im.code_refs(a)][:8], 'data', [hex(x) for x in im.data_refs(a)][:8])
    elif cmd == 'dis':
        im.dis(int(args[0], 0), int(args[1], 0) if len(args) > 1 else 0x80)
    elif cmd == 'refs':
        for a in args:
            t = int(a, 0)
            print(a, 'code', [hex(x) for x in im.code_refs(t)][:20], 'calls', [hex(x) for x in im.callers(t)][:20],
                  'data', [hex(x) for x in im.data_refs(t)][:20])


if __name__ == '__main__':
    main(sys.argv[1:])
