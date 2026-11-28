"""Mod switches: game code generated both ways, the mod's and the original, picked at run time by a C++ flag.

Kinds (GAME.MOD_SWITCHES entries: address, original bytes, the mod's bytes, flag[, option]):
- operand: a same-length operand edit of one plain instruction; both must have equal effects.
- value (option 'value'): an operand edit whose immediate or displacement is read at run time from the patched
  guest bytes, which the port writes as the mod does.
- flow: the mod's bytes decode to other instructions inside the original ones (jcc to jmp, NOP runs, a changed
  compare); the original analysis must stay valid for them.
- call (option 'call NAME [REG...] [reads REG...]', no mod bytes): a direct call becomes a call of the C++ function
  NAME, which writes the registers before 'reads' and otherwise keeps the callee's convention. The registers after
  'reads' are passed in cpu for the call (their cpu copies are put back after it).
"""

import re

from capstone import x86 as X

from . import codegen
from .analysis import REG_SHIFT
from .codegen import ctx as C


class Switch:
    def __init__(self, kind, flag, start, span, mods, value=None, call=None, reads=()):
        self.kind = kind
        self.flag = flag
        self.start = start          # index of the first original instruction
        self.span = span            # original instructions replaced
        self.mods = mods            # (original index, mod instruction)
        self.value = value          # (field address, bytes, signed) of a value switch
        self.call = call            # C++ function of a call switch
        self.reads = reads          # registers it reads in cpu
        self.sentinel_mods = {}     # value switch: sentinel -> the instruction decoded with it


class ModCtx(C.Ctx):
    """Emission of mod instructions: flags they set stay in mf_ locals of the switch block."""

    def __init__(self, fn, function_names):
        super().__init__(fn, function_names, record=False)
        self.local = set()

    def set_flags(self, values, kind=None, bits=32, operands=None, may=False):
        if kind:
            values = dict(C.lazy_flags(kind, bits, operands), **values)
            values = {f: values[f] for f in C.LAZY_KINDS[kind][1]}
        lines = []
        for f, expr in values.items():
            if f != 'fc':
                lines.append('mf_%s = %s;' % (f, expr))
                self.local.add(f)
        return lines

    def flag(self, f):
        return 'mf_' + f if f in self.local else super().flag(f)

    def cond(self, cc):
        cc = C.CC_ALIASES[cc]
        flags = C.CONDITIONS[cc]
        if all(f in self.local for f in flags):
            return C.generic_condition(cc, {f: 'mf_' + f for f in flags})
        if any(f in self.local for f in flags):
            raise AssertionError('0x%x: a mod condition reads flags of two producers' % self.ins.address)
        return super().cond(cc)


def effects(fn, index, ins):
    ctx = C.Ctx(fn, {}, record=True)
    ctx.begin(index)
    ctx.ins = ins
    codegen.generate(ctx, ins)
    return ctx.effects


def is_control(ins):
    m = ins.mnemonic.split()[-1]
    return m in ('call', 'ret', 'jmp', 'loop', 'loope', 'loopne', 'jecxz', 'jcxz') or m.startswith('j')


def fail(address, text):
    raise SystemExit('mod switch 0x%x: %s' % (address, text))


def build(image, cs, program, entries):
    """Instruction address -> Switch for every function holding a switched address."""
    switches = {}
    problems = []
    holders = {}
    if entries:
        for fn in program.functions.values():
            for i, ins in enumerate(fn.instructions):
                for a in range(ins.address, ins.address + ins.size):
                    holders.setdefault(a, set()).add(fn)
    for entry in entries:
        try:
            add(image, cs, holders, entry, switches)
        except SystemExit as e:
            problems.append(str(e))
    if problems:
        raise SystemExit('\n'.join(problems))
    return switches


def add(image, cs, holders, entry, switches):
    """Adds the switches of one GAME.MOD_SWITCHES entry."""
    address, original, patched, flag = entry[:4]
    option = entry[4] if len(entry) > 4 else ''
    original = bytes.fromhex(original)
    if image.read(address, len(original)) != original:
        fail(address, 'bytes %s, expected %s' % (image.read(address, len(original)).hex(), original.hex()))
    end = address + len(original)
    found = False
    for fn in sorted(set().union(*(holders.get(a, set()) for a in range(address, end))), key=lambda f: f.entry_address):
        unit = [i for i, ins in enumerate(fn.instructions) if ins.size and ins.address < end
                and address < ins.address + ins.size]
        if not unit:
            continue
        found = True
        unit = list(range(min(unit), max(unit) + 1))
        if option.startswith('call'):
            switch = call_switch(fn, unit, address, option, flag)
        else:
            switch = byte_switch(image, cs, fn, unit, address, bytes.fromhex(patched), flag, option)
        switches[(fn.entry_address, fn.instructions[unit[0]].address)] = switch
    if not found:
        fail(address, 'no generated instruction holds it')


def call_switch(fn, unit, address, option, flag):
    words = option.split()
    name, rest = words[1], words[2:]
    writes = rest[:rest.index('reads')] if 'reads' in rest else rest
    reads = rest[rest.index('reads') + 1:] if 'reads' in rest else []
    if any(r not in C.REGS for r in writes + reads):
        fail(address, 'unknown register in %r' % option)
    (i,) = unit
    ins = fn.instructions[i]
    if fn.branch[i][0] != 'call':
        fail(address, '%s %s is no direct call' % (ins.mnemonic, ins.op_str))
    may = fn.callee_may_def(i)
    after = fn.live_out[i]
    written = sum(C.REG_BIT[r] for r in writes)
    stale = written & (after >> REG_SHIFT) & ~(may >> REG_SHIFT)
    if stale:
        fail(address, 'the caller keeps %s across the call, which %s writes'
             % (' '.join(r for r in C.REGS if stale & C.REG_BIT[r]), name))
    must = fn.callee_must_def(i) >> REG_SHIFT
    read_after = (after >> REG_SHIFT) & must & ~written
    if read_after:
        fail(address, 'registers the callee always writes are read after the call and %s does not write them: %s'
             % (name, ' '.join(r for r in C.REGS if read_after & C.REG_BIT[r])))
    if after & C.CPU_FLAG_MASK:
        fail(address, 'flags are read after the call')
    return Switch('call', flag, i, 1, [], call=name, reads=reads)


def byte_switch(image, cs, fn, unit, address, patched, flag, option):
    first = fn.instructions[unit[0]]
    last = fn.instructions[unit[-1]]
    start, end = first.address, last.address + last.size
    data = bytearray(image.read(start, end - start))
    data[address - start:address - start + len(patched)] = patched
    mods = []
    p = start
    for ins in cs.disasm(bytes(data), start):
        owner = next((j for j in unit if fn.instructions[j].address <= ins.address
                      and ins.address + ins.size <= fn.instructions[j].address + fn.instructions[j].size), None)
        if owner is None:
            fail(address, 'the mod\'s %s %s crosses an original instruction' % (ins.mnemonic, ins.op_str))
        mods.append((owner, ins))
        p = ins.address + ins.size
        if p >= end:
            break
    if p != end:
        fail(address, 'the mod\'s bytes do not decode up to 0x%x' % end)
    same = len(mods) == len(unit) and all(m.mnemonic == fn.instructions[j].mnemonic and m.size ==
                                          fn.instructions[j].size for j, m in mods)
    if option == 'value':
        if not (same and len(unit) == 1):
            fail(address, 'a value switch must keep the instruction')
        return value_switch(image, cs, fn, unit[0], address, len(patched), mods[0][1], flag)
    if same and len(unit) == 1:
        operand_switch(fn, unit[0], address, mods[0][1])
        return Switch('operand', flag, unit[0], 1, mods)
    flow_switch(fn, unit, address, mods)
    return Switch('flow', flag, unit[0], len(unit), mods)


def operand_switch(fn, i, address, mod):
    ins = fn.instructions[i]
    if fn.exits[i] or is_control(ins):
        fail(address, '%s %s -> %s %s, only same-length operand edits of plain instructions'
             % (ins.mnemonic, ins.op_str, mod.mnemonic, mod.op_str))
    a, b = effects(fn, i, ins), effects(fn, i, mod)
    if tuple(getattr(a, k) for k in C.Effects.__slots__) != tuple(getattr(b, k) for k in C.Effects.__slots__):
        fail(address, 'the mod\'s instruction reads or writes other registers or flags')


def field_of(ins, address, size):
    """(offset, size) of the immediate or displacement holding the patched bytes."""
    offset = address - ins.address
    for o, s, kind in ((ins.imm_offset, ins.imm_size, 'imm'), (ins.disp_offset, ins.disp_size, 'disp')):
        if s and o <= offset and offset + size <= o + s:
            return o, s, kind
    return None


def value_switch(image, cs, fn, i, address, size, mod, flag):
    ins = fn.instructions[i]
    operand_switch(fn, i, address, mod)
    field = field_of(mod, address, size)
    if field is None:
        fail(address, 'the patched bytes are not one immediate or displacement')
    offset, width, kind = field
    if kind == 'disp' and mod.mnemonic != 'lea':
        fail(address, 'a variable displacement of a memory access would move stack slots')
    raw = bytearray(image.read(ins.address, ins.size))
    probe = bytearray(raw)
    probe[offset:offset + width] = (0x80 << (8 * (width - 1))).to_bytes(width, 'little')
    decoded = next(cs.disasm(bytes(probe), ins.address))
    values = [op.imm for op in decoded.operands if op.type == X.X86_OP_IMM] if kind == 'imm' else \
        [op.mem.disp for op in decoded.operands if op.type == X.X86_OP_MEM]
    signed = width < 4 and any((v & 0xffffffff) >= 0x80000000 for v in values)
    switch = Switch('value', flag, i, 1, [(i, mod)], value=(ins.address + offset, width, signed))
    for sentinel in sentinels(width):
        data = bytearray(raw)
        data[offset:offset + width] = sentinel.to_bytes(width, 'little')
        candidate = next(cs.disasm(bytes(data), ins.address))
        operand_switch(fn, i, address, candidate)
        switch.sentinel_mods[sentinel] = candidate
    return switch


def sentinels(width):
    return {1: (0x5a, 0x6b), 2: (0x5a5a, 0x6b6b), 4: (0x1234567a, 0x2468ace0)}[width]


def flow_switch(fn, unit, address, mods):
    for j in unit:
        ins = fn.instructions[j]
        if fn.branch[j][0] == 'skip' or ins.address in fn.thread_segments:
            fail(address, 'skipped or thread-segment instruction at 0x%x' % ins.address)
        if j != unit[0] and fn.is_label(j):
            fail(address, '0x%x inside the switched bytes is a jump target' % ins.address)
        if ins.mnemonic.startswith('f') or fn.branch[j][0] not in ('next', 'label') or fn.exits[j]:
            fail(address, '%s %s: only plain instructions and jumps inside the function' % (ins.mnemonic, ins.op_str))
    orig_use = orig_def = orig_full = orig_flags = 0
    defined_before = {}
    for j in unit:
        e = fn.effects[j]
        defined_before[j] = orig_flags
        orig_use |= e.reg_use | e.reg_partial
        orig_def |= e.reg_def | e.reg_partial
        orig_full |= e.reg_def
        orig_flags |= e.flag_def | e.flag_maydef
    mod_use = mod_def = mod_full = mod_flags = 0
    for j, ins in mods:
        e = effects(fn, j, ins)
        if e.boundary or ins.mnemonic.startswith('f') or e.flag_use & C.FLAG_BIT['fc'] or e.flag_def & C.FLAG_BIT['fc']:
            fail(address, '%s %s: no calls, x87 or flag boundaries in a flow switch' % (ins.mnemonic, ins.op_str))
        if is_control(ins):
            m = ins.mnemonic
            target = ins.operands[0].imm & 0xffffffff if ins.operands[0].type == X.X86_OP_IMM else None
            if m not in ('jmp',) and not (m.startswith('j') and m[1:] in C.CC_ALIASES):
                fail(address, '%s: only jmp and jcc' % m)
            if fn.branch[j] != ('label', target):
                fail(address, '%s %s must jump where the original 0x%x jumps' % (m, ins.op_str,
                                                                                 fn.instructions[j].address))
            if m != 'jmp':
                om = fn.instructions[j].mnemonic
                if not (om.startswith('j') and om[1:] in C.CC_ALIASES) or \
                        set(C.CONDITIONS[C.CC_ALIASES[m[1:]]]) != set(C.CONDITIONS[C.CC_ALIASES[om[1:]]]):
                    fail(address, '%s reads other flags than the original %s' % (m, om))
        outside = e.flag_use & ~mod_flags & C.CPU_FLAG_MASK
        if outside & ~fn.effects[j].flag_use or outside & defined_before[j]:
            fail(address, '%s %s reads flags the original analysis does not serve there' % (ins.mnemonic, ins.op_str))
        mod_use |= e.reg_use | e.reg_partial
        mod_def |= e.reg_def | e.reg_partial
        mod_full |= e.reg_def
        mod_flags |= e.flag_def | e.flag_maydef
    if mod_use & ~orig_use or mod_def & ~orig_def:
        fail(address, 'the mod reads or writes registers the original does not')
    if mod_def & C.REG_BIT['esp']:
        fail(address, 'the mod moves esp')
    after = fn.live_out[unit[-1]]
    if after & (orig_flags | mod_flags) & C.CPU_FLAG_MASK:
        fail(address, 'flags the switched code sets are read after it')
    dropped = ((orig_def & ~mod_def) | (orig_full & ~mod_full)) & (after >> REG_SHIFT)
    for r in C.REGS:
        if dropped & C.REG_BIT[r] and not defined_in_locals(fn, unit[0], C.REG_BIT[r]):
            fail(address, '%s, which the mod no longer writes, is read after it and may be stale in its local' % r)


def defined_in_locals(fn, start, bit):
    """Every path to start writes the register whole with no call or exit after: its local holds the value."""
    seen = set()
    stack = list(fn.pred[start])
    if start == fn.entry:
        return False
    while stack:
        p = stack.pop()
        if p in seen:
            continue
        seen.add(p)
        e = fn.effects[p]
        if e.reg_def & bit:
            continue
        if fn.is_call(p) or e.boundary or fn.exits[p] or p == fn.entry:
            return False
        stack.extend(fn.pred[p])
    return True


# ---- emission ---------------------------------------------------------------------------------------------------

LITERAL = re.compile(r'(x86::reg(?:8|16)\(0x[0-9a-f]+\)|0x[0-9a-f]+u)')


def literal_value(token):
    if token.endswith('u'):
        return int(token[2:-1], 16), 32
    bits = 8 if token.startswith('x86::reg8') else 16
    return int(token[token.index('(') + 3:-1], 16), bits


def value_lines(switch, generate):
    """The mod instruction's code with its field read from guest memory; generate(bytes value) -> lines."""
    field, width, signed = switch.value
    load = 'rc::ld%d(0x%xu)' % (width * 8, field)
    if signed:
        load = 'x86::reg32(x86::sreg%d(%s))' % (width * 8, load)
    elif width < 4:
        load = 'x86::reg32(%s)' % load
    a, b = sentinels(width)
    la, lb = generate(a), generate(b)
    if len(la) != len(lb):
        raise AssertionError('value switch 0x%x: the code depends on the value' % field)

    def extended(v):
        if signed and v & (0x80 << (8 * (width - 1))):
            v -= 1 << (8 * width)
        return v & 0xffffffff

    out = []
    for x, y in zip(la, lb):
        tx, ty = LITERAL.split(x), LITERAL.split(y)
        if len(tx) != len(ty):
            raise AssertionError('value switch 0x%x: the code depends on the value' % field)
        line = []
        for k, (p, q) in enumerate(zip(tx, ty)):
            if p == q:
                line.append(p)
                continue
            if k % 2 == 0:
                raise AssertionError('value switch 0x%x: the code depends on the value' % field)
            (va, bits), (vb, bits_b) = literal_value(p), literal_value(q)
            mask = (1 << bits) - 1
            if bits != bits_b or va != extended(a) & mask or vb != extended(b) & mask:
                raise AssertionError('value switch 0x%x: %s is not the field' % (field, p))
            line.append('(%s)' % load if bits == 32 else 'x86::reg%d(%s)' % (bits, load))
        out.append(''.join(line))
    return out


def block(flag, mod_lines, orig_lines):
    return (['if (%s)' % flag, '{'] + ['    ' + l for l in mod_lines] + ['}', 'else', '{']
            + ['    ' + l for l in orig_lines] + ['}'])
