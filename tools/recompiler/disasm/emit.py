"""Writes one recompiled function as C++."""

import re

from . import codegen
from .codegen import ctx as C
from . import modswitch


# (function entry, instruction address) -> modswitch.Switch: generated both ways, the flag picks (generate.py).
MOD_SWITCHES = {}


def raw_bytes(ins):
    """Comment text of a decoded instruction from its own bytes."""
    return '%08x  %s%s %s %s' % (ins.address, bytes(ins.bytes).hex(), ' ' * max(1, 23 - ins.size * 2), ins.mnemonic,
                                 ins.op_str)


def raw(module, fn, ins):
    """Comment text of an instruction: address, bytes, disassembly."""
    if not ins.size:
        return '%s %s' % (ins.__class__.__name__, ins.op_str)
    data = module.image.read(ins.address, ins.size)
    return '%08x  %s%s %s %s' % (ins.address, data.hex(), ' ' * max(1, 23 - ins.size * 2), ins.mnemonic, ins.op_str)


def instruction_code(ctx, i, ins, expected):
    """Statements of one instruction generated at index i (ctx.ins may be a mod instruction)."""
    code = codegen.generate(ctx, ins)
    return ctx.slot_checks + code


def switched(module, fn, function_names, ctx, i, ins, code, expected, switch):
    """Both ways of a mod switch starting at instruction i; code is the original instruction's."""
    if switch.kind == 'call':
        name = ctx.function_name(fn.branch[i][1])
        call = '%s(app, cpu);' % name
        if sum(l.count(call) for l in code) != 1:
            raise AssertionError('0x%x: the call switch finds no single call of %s' % (ins.address, name))
        native = '%s(app, cpu);' % switch.call
        if switch.reads:
            keep = ' '.join('const x86::reg32 keep_%s = cpu.%s; cpu.%s = %s;' % (r, r, r, ctx.reg(r))
                            for r in switch.reads)
            back = ' '.join('cpu.%s = keep_%s;' % (r, r) for r in switch.reads)
            native = '{ %s %s %s }' % (keep, native, back)
        mod = [l.replace(call, native) for l in code]
        return modswitch.block(switch.flag, mod, code)
    if switch.kind in ('operand', 'value'):
        (_, mod), = switch.mods

        def generate(value=None):
            m = mod if value is None else switch.sentinel_mods[value]
            ctx.begin(i)
            ctx.ins = m
            lines = instruction_code(ctx, i, m, expected)
            if ctx.used_slots != expected.get(i, set()):
                raise AssertionError('0x%x: the mod switch changes promoted stack slot accesses' % ins.address)
            return lines
        modded = modswitch.value_lines(switch, generate) if switch.kind == 'value' else generate()
        return modswitch.block(switch.flag, ['// %s' % raw_bytes(mod)] + modded, code)
    orig = list(code)
    for j in range(i + 1, i + switch.span):
        other = fn.instructions[j]
        ctx.begin(j)
        orig += ['// %s' % raw(module, fn, other)] + instruction_code(ctx, j, other, expected)
        if ctx.used_slots != expected.get(j, set()):
            raise AssertionError('0x%x: promoted stack slot accesses %s, generated %s'
                                 % (other.address, expected.get(j), ctx.used_slots))
    mctx = modswitch.ModCtx(fn, function_names)
    mod_lines = []
    used = {}
    for j, m in switch.mods:
        mctx.begin(j)
        mctx.ins = m
        mod_lines += ['// %s' % raw_bytes(m)] + instruction_code(mctx, j, m, expected)
        used.setdefault(j, set()).update(mctx.used_slots)
    for j in range(i, i + switch.span):
        if used.get(j, set()) != expected.get(j, set()):
            raise AssertionError('0x%x: the mod switch changes promoted stack slot accesses' % ins.address)
    declarations = ['bool mf_%s = false;' % f for f in C.CPU_FLAGS if f in mctx.local]
    return modswitch.block(switch.flag, declarations + mod_lines, orig)


def function_body(module, fn, function_names):
    """Body lines of the function after its local declarations."""
    ctx = C.Ctx(fn, function_names, record=False)
    sub = fn.sub
    lines = []
    frame = getattr(fn, 'frame', None)
    expected = {}
    if frame is not None:
        for (i, key) in list(frame.sites) + list(frame.stack_sites):
            expected.setdefault(i, set()).add(key)
        for slot in sorted(frame.slots.values(), key=lambda s: s.name):
            if slot.entry_load:
                offset = 'esp + 0x%xu' % slot.k if slot.k >= 0 else 'esp - 0x%xu' % -slot.k
                lines.append('  %s = rc::load<%s>(%s);' % (slot.name, slot.ctype, offset))
    if sub.dynamic_labels:
        lines += ['  goto start;', 'dynamic_jump:', '  switch(cpu.ip)', '  {', 'start:']
    if fn.entry != 0:
        lines.append('    goto L_entry_0x%08x;' % fn.entry_address)
    fallthrough = False
    skip_to = 0
    for i, ins in enumerate(fn.instructions):
        if i < skip_to:
            fallthrough = ins.mnemonic not in ('jmp', 'ret')
            continue
        # Labels go before the first instruction at an address (an added jump shares its predecessor's address).
        first = fn.index_of[ins.address] == i
        if first and ins.address in fn.dynamic_targets:
            if fallthrough:
                lines.append('  [[fallthrough]];')
            lines.append('  case 0x%08x:' % ins.address)
        if first and ins.address in fn.static_targets:
            lines.append('L_0x%08x:' % ins.address)
        if fn.entry != 0 and i == fn.entry:
            lines.append('L_entry_0x%08x:' % ins.address)
        if first and ins.address in fn.thread_segments:
            lines += ['    app->unlockContext(cpu);', '    win32::Thread::sleep(0);', '    app->lockContext(cpu);']
        lines.append('    // %s' % raw(module, fn, ins))
        if fn.branch[i][0] == 'skip':
            lines.append('    // skipped')
            code = []
        else:
            ctx.begin(i)
            code = codegen.generate(ctx, ins)
            if ctx.used_slots != expected.get(i, set()):
                raise AssertionError('0x%x: promoted stack slot accesses %s, generated %s'
                                     % (ins.address, expected.get(i), ctx.used_slots))
            code = ctx.slot_checks + code
            switch = MOD_SWITCHES.get((fn.entry_address, ins.address))
            if switch is not None:
                code = switched(module, fn, function_names, ctx, i, ins, code, expected, switch)
                skip_to = i + switch.span
        lines += ['    ' + l for l in code]
        fallthrough = ins.mnemonic not in ('jmp', 'ret')
    last = fn.n - 1
    if fn.branch[last][0] not in ('ret',) and not (fn.branch[last][0] in ('label', 'tail', 'dyntail', 'switch')
                                                   and fn.instructions[last].mnemonic == 'jmp'):
        ctx.begin(last)
        ctx.fdepth = fn.fpu_exit_depth(last)
        lines += ['    ' + l for l in ctx.exit_sync('exit')]
    if sub.dynamic_labels:
        lines += ['  default:', '    x86::guestTrap(cpu.ip, "jump table target outside the function");', '  }']
    return lines


def declarations(fn, body):
    """Locals used by the body: flags (loaded when live on entry) and saved operands of lazy producers."""
    text = '\n'.join(body)
    used = set(re.findall(r'\b(f_[a-z]{2}|lz[a-z]+_[0-9a-f]+)\b', text))
    lines = []
    if fn.local_registers:
        words = set(re.findall(r'\b(e[a-d]x|e[sd]i|e[sb]p)\b', text))
        for r in C.REGS:
            if r in words:
                lines.append('  [[maybe_unused]] x86::reg32 %s = cpu.%s;' % (r, r))
    if re.search(r'\bfpu_count\b', text):
        lines.append('  const x86::reg32 fpu_count = cpu.fpu.count;')
    for n in sorted(set(int(x) for x in re.findall(r'\bfp(\d+)\b', text))):
        lines.append('  [[maybe_unused]] x86::Float fp%d = 0;' % n)
    entry = fn.entry_loads()
    lines += fn.audit_reload_lines(sum(C.FLAG_BIT[f] for f in entry if 'f_' + f in used), fn.entry_address)
    for f in C.CPU_FLAGS:
        name = 'f_' + f
        if name in used:
            value = 'cpu.flags.%s' % f if f in entry else 'false'
            lines.append('  bool %s = %s;' % (name, value))
    for type_, name in fn.lazy_locals():
        if name in used:
            lines.append('  %s %s = 0;' % (type_, name))
    frame = getattr(fn, 'frame', None)
    if frame is not None:
        for slot in sorted(frame.slots.values(), key=lambda s: s.name):
            lines.append('  [[maybe_unused]] %s %s = 0;' % (slot.ctype, slot.name))
    if re.search(r'\bsb\b', text):
        lines.append('  RC_STACK_BASE(sb);')
    if re.search(r'\besp0\b', text):
        lines.append('  const x86::reg32 esp0 = cpu.esp;')
    return lines


def write_entry(out, header, module, program, fn, name, qualifier, declare, app_type, function_names):
    """Declares and writes one guest function. With a native override the translated body becomes <name>_guest,
    and the entry calls native::<name> through RC_NATIVE (include/native.h)."""
    parameters = '(%s* app, x86::CPU& cpu)' % app_type
    header.write('%svoid %s%s;\n' % (declare, name, parameters))
    if fn.entry_address in program.native_overrides:
        header.write('%svoid %s_guest%s;\n' % (declare, name, parameters))
        out.write('namespace native { void %s(win32::WinApplication* app, x86::CPU& cpu); }\n' % name)
        out.write('void %s%s%s\n{\n    RC_NATIVE(0x%x, native::%s, %s_guest);\n}\n\n'
                  % (qualifier, name, parameters, fn.entry_address, name, name))
        name += '_guest'
    write_function(out, module, fn, 'void %s%s%s' % (qualifier, name, parameters), function_names)


def write_function(out, module, fn, signature, function_names):
    body = function_body(module, fn, function_names)
    out.write('%s\n{\n  RC_USE(cpu);\n  RC_USE(app);\n' % signature)
    for line in declarations(fn, body) + body:
        out.write(line + '\n')
    out.write('}\n\n')
