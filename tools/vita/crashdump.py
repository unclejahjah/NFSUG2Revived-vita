#!/usr/bin/env python3
"""Reads a PS Vita crash dump (ux0:data/psp2core-*.psp2dmp, gzip'd ELF core) or a crash report of the port
(ux0:data/NeedForSpeedUnderground2/crashes/crash-*.txt) and prints what crashed.

    python3 tools/vita/crashdump.py psp2core-....psp2dmp [--elf build-vita/NFSU2VITA.elf] [--all]
    python3 tools/vita/crashdump.py crash-20261121-233122.txt --elf build-vita/NFSU2VITA.elf

Works on incomplete dumps (.psp2dmp.tmp) as far as their data goes. With --elf (the unstripped build that crashed),
addresses in the game module are named: functions sub_XXXXXX are the translated SPEED2.EXE function at guest XXXXXX.
The port's crash ring (src/platform/crashlog.cpp) is found in the dump's memory and printed: the last log lines,
info ones included, and the lines that were still waiting for the log writer.
"""
import argparse
import bisect
import gzip
import struct
import subprocess
import sys
import zlib

STOP = {0: 'none', 0x30002: 'undefined instruction', 0x30003: 'prefetch abort', 0x30004: 'data abort',
        0x60080: 'division by zero'}
STATUS = {1: 'running', 2: 'ready', 4: 'standby', 8: 'waiting', 16: 'dormant', 32: 'deleted', 64: 'dead',
          128: 'stagnant', 256: 'suspended'}
RING_MAGIC = b'NFSU2CRASHRING01'


def u16(b, o):
    return struct.unpack_from('<H', b, o)[0]


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def cstr(b, o, n=None):
    end = b.find(b'\0', o, o + n if n else len(b))
    return b[o:end if end >= 0 else (o + n if n else len(b))].decode('latin-1')


def load(path):
    raw = open(path, 'rb').read()
    if raw[:2] == b'\x1f\x8b':
        d = zlib.decompressobj(31)
        try:
            raw = d.decompress(raw)
        except zlib.error:
            pass  # an incomplete dump: keep what decompressed
        truncated = not d.eof
    else:
        truncated = False
    return raw, truncated


class Core:
    def __init__(self, data):
        self.data = data
        if data[:4] != b'\x7fELF':
            raise ValueError('not an ELF core')
        phoff, phentsize, phnum = u32(data, 0x1c), u16(data, 0x2a), u16(data, 0x2c)
        self.notes, self.loads = {}, []
        for k in range(phnum):
            o = phoff + k * phentsize
            if o + 32 > len(data):
                break
            ptype, off, vaddr, _, filesz = struct.unpack_from('<IIIII', data, o)
            if ptype == 4:
                self._notes(off, filesz)
            elif ptype == 1 and filesz:
                self.loads.append((vaddr, off, min(filesz, max(0, len(data) - off))))
        self.loads.sort()
        self.modules = self._modules()
        self.threads = self._threads()

    def _notes(self, off, size):
        d, p, end = self.data, off, min(off + size, len(self.data))
        while p + 12 <= end:
            namesz, descsz, _ = struct.unpack_from('<III', d, p)
            name = cstr(d, p + 12, namesz)
            desc = p + 12 + ((namesz + 3) & ~3)
            self.notes[name] = d[desc:desc + descsz]
            p = desc + ((descsz + 3) & ~3)

    def _modules(self):
        d, mods = self.notes.get('MODULE_INFO', b''), []
        if len(d) < 8:
            return mods
        off = 8
        for _ in range(u32(d, 4)):
            if off + 0x50 > len(d):
                break
            name, nseg = cstr(d, off + 0x24, 28), u32(d, off + 0x4c)
            off += 0x50
            segs = []
            for s in range(nseg):
                segs.append((u32(d, off + 8), u32(d, off + 12), u32(d, off + 4), s + 1))
                off += 0x14
            off += 0x10
            mods.append((name, segs))
        return mods

    def _threads(self):
        d, threads = self.notes.get('THREAD_INFO', b''), {}
        if len(d) >= 8:
            off = 8
            for _ in range(u32(d, 4)):
                sz = u32(d, off)
                t = d[off:off + sz]
                threads[u32(t, 4)] = {'id': u32(t, 4), 'name': cstr(t, 8, 32), 'status': u16(t, 0x30),
                                      'stop': u32(t, 0x74), 'pc': u32(t, 0x9c), 'regs': None}
                off += sz
        r = self.notes.get('THREAD_REG_INFO', b'')
        if len(r) >= 8:
            off = 8
            for _ in range(u32(r, 4)):
                sz = u32(r, off)
                t = r[off:off + sz]
                tid = u32(t, 4)
                if tid in threads:
                    threads[tid]['regs'] = [u32(t, 8 + 4 * k) for k in range(16)]
                    threads[tid]['cpsr'] = u32(t, 0x48) if sz >= 0x4c else None
                off += sz
        return list(threads.values())

    def where(self, addr):
        for name, segs in self.modules:
            for start, size, attr, num in segs:
                if start <= addr < start + size:
                    return name, num, addr - start
        return None

    def read(self, addr, size):
        for vaddr, off, filesz in self.loads:
            if vaddr <= addr and addr + size <= vaddr + filesz:
                return self.data[off + addr - vaddr:off + addr - vaddr + size]
        return None

    def find(self, needle):
        for vaddr, off, filesz in self.loads:
            p = self.data.find(needle, off, off + filesz)
            if p >= 0:
                return vaddr + p - off
        return None


class Symbols:
    def __init__(self, elf):
        self.elf, self.addrs, self.names = elf, [], []
        out = subprocess.run(['nm', '-n', '-C', '--defined-only', elf], capture_output=True, text=True).stdout
        for line in out.splitlines():
            parts = line.split(' ', 2)
            if len(parts) == 3 and parts[1] in 'tTwW':
                self.addrs.append(int(parts[0], 16))
                self.names.append(parts[2])
        # The game module's first segment is linked at the ELF's first PT_LOAD address.
        self.base = self.addrs[0] & ~0xfff if self.addrs else 0
        try:
            hdr = subprocess.run(['readelf', '-lW', elf], capture_output=True, text=True).stdout
            for line in hdr.splitlines():
                if line.strip().startswith('LOAD'):
                    self.base = int(line.split()[2], 16)
                    break
        except OSError:
            pass

    def name(self, offset):
        a = self.base + offset
        k = bisect.bisect_right(self.addrs, a & ~1) - 1
        if k < 0:
            return None
        return '%s+0x%x' % (self.names[k], (a & ~1) - self.addrs[k])


def describe(core, syms, addr):
    w = core.where(addr)
    if not w:
        return '0x%08x' % addr
    name, seg, off = w
    text = '0x%08x %s@%d+0x%x' % (addr, name, seg, off)
    if syms and seg == 1 and name.lower().startswith(('nfsu2vita', 'eboot', 'game')):
        s = syms.name(off)
        if s:
            text += ' ' + s
    return text


def ring(core):
    at = core.find(RING_MAGIC)
    if at is None:
        return None
    head = core.read(at, 32)
    size, used, wrapped, _ = struct.unpack_from('<IIII', head, 16)
    body = core.read(at + 32, size) or b''
    text = (body[used:] + body[:used]) if wrapped else body[:used]
    return text.decode('latin-1', 'replace')


def report(path, syms):
    """A crash report: each "(NAME@1+0xOFFSET)" in the game module gets its function name."""
    import re
    pattern = re.compile(r'\((NFSU2VITA[^@)]*)@1\+0x([0-9a-f]+)\)')
    for line in open(path, encoding='latin-1'):
        line = line.rstrip('\n')
        if syms:
            line = pattern.sub(lambda m: '(%s@1+0x%s %s)' % (m.group(1), m.group(2),
                                                            syms.name(int(m.group(2), 16)) or '?'), line)
        print(line)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dump', help='a psp2core-*.psp2dmp (or .tmp) or a crash-*.txt')
    ap.add_argument('--elf', help='the NFSU2VITA.elf (unstripped, in the build folder) of the build that crashed')
    ap.add_argument('--stack', type=int, default=48, help='stack words scanned for return addresses')
    ap.add_argument('--all', action='store_true', help='every thread, not only the crashed ones')
    a = ap.parse_args()
    syms = Symbols(a.elf) if a.elf else None
    if open(a.dump, 'rb').read(9) == b'NeedForSp':
        report(a.dump, syms)
        return 0
    data, truncated = load(a.dump)
    core = Core(data)
    print('dump %s: %u bytes%s' % (a.dump, len(data), ', INCOMPLETE (the system did not finish writing it)'
                                   if truncated else ''))
    print('modules:', ', '.join(m[0] for m in core.modules) or '(none read)')
    crashed = [t for t in core.threads if t['stop']]
    print('threads: %u, crashed: %u' % (len(core.threads), len(crashed)))
    for t in core.threads:
        flag = ' <== %s' % STOP.get(t['stop'], hex(t['stop'])) if t['stop'] else ''
        print('  %-31s 0x%08x %-10s pc %s%s' % (t['name'], t['id'], STATUS.get(t['status'], hex(t['status'])),
                                              describe(core, syms, t['pc']), flag))
    for t in (core.threads if a.all else crashed):
        print('\n=== %s (0x%08x): %s ===' % (t['name'], t['id'], STOP.get(t['stop'], hex(t['stop']))))
        r = t['regs']
        if not r:
            print('  no registers in the dump')
            continue
        for k in range(0, 13, 4):
            print('  ' + '  '.join('r%-2d %08x' % (j, r[j]) for j in range(k, min(k + 4, 13))))
        print('  sp  %08x' % r[13])
        print('  lr  %s' % describe(core, syms, r[14]))
        print('  pc  %s' % describe(core, syms, r[15]))
        print('  return addresses on the stack:')
        found = 0
        for k in range(a.stack * 8):
            w = core.read(r[13] + 4 * k, 4)
            if w is None:
                print('    (stack not in the dump past sp+0x%x)' % (4 * k))
                break
            v = u32(w, 0)
            loc = core.where(v)
            if loc and loc[1] == 1 and v & 1:
                print('    sp+0x%03x %s' % (4 * k, describe(core, syms, v)))
                found += 1
                if found >= a.stack:
                    break
    text = ring(core)
    if text is None:
        print('\n(no crash ring in this dump: an older build, or the memory is not in it)')
    else:
        print('\n=== last log lines (crash ring) ===')
        print(text.rstrip('\n'))


if __name__ == '__main__':
    sys.exit(main())
