"""Jumps, calls and returns."""

from capstone import x86 as X


def _target(ctx, edge):
    """Statements transferring control to the target of the current jump."""
    kind = ctx.fn.branch[ctx.index]
    if kind[0] == 'label':
        jump = ['goto L_0x%08x;' % kind[1]]
        if kind[1] <= ctx.ins.address and ctx.fn.program.preempt_loops:
            # A loop: preemption point, so a spin on state that a timer callback or a higher-priority thread
            # changes lets that thread take the execution lock.
            jump.insert(0, 'app->allowPreemption(cpu);')
        return jump
    if kind[0] == 'tail':
        return ctx.exit_sync(edge) + ['return %s(app, cpu);' % ctx.function_name(kind[1])]
    op = ctx.ins.operands[0]
    if kind[0] == 'switch':
        return ['cpu.ip = %s;' % ctx.val(op, 32), 'goto dynamic_jump;']
    target = ctx.val(op, 32)
    ctx.boundary('dyncall')
    buffer = ctx.fn.program.longjmp_jumps.get(ctx.ins.address)
    if buffer:
        # longjmp: resumes after the setjmp call that filled the guest jmp_buf.
        return ctx.exit_sync(edge) + ['rc::longJump(app, cpu, %s);' % ctx.reg(buffer)]
    return ctx.exit_sync(edge) + ['return app->dynamic_call(%s, cpu);' % target]


def cg_jmp(ctx, *operands):
    return _target(ctx, 'exit')


def cg_jcc(ctx, cc):
    return ['if (%s)' % ctx.cond(cc), '{'] + ['    ' + l for l in _target(ctx, 'taken')] + ['}']


def cg_jcxz(ctx, *operands):
    return ['if (%s == 0)' % ctx.reg('cx'), '{'] + ['    ' + l for l in _target(ctx, 'taken')] + ['}']


def cg_jecxz(ctx, *operands):
    return ['if (%s == 0)' % ctx.reg('ecx'), '{'] + ['    ' + l for l in _target(ctx, 'taken')] + ['}']


def _loop(ctx, condition):
    """loop/loope/loopne: ecx is decremented without changing flags."""
    jump = ['if (%s != 0%s)' % (ctx.reg('ecx'), condition), '{'] + ['    ' + l for l in _target(ctx, 'taken')] + ['}']
    return ctx.set_reg('ecx', '%s - 1' % ctx.reg('ecx')) + jump


def cg_loop(ctx, *operands):
    return _loop(ctx, '')


def cg_loope(ctx, *operands):
    return _loop(ctx, ' && %s' % ctx.flag('zf'))


def cg_loopne(ctx, *operands):
    return _loop(ctx, ' && !%s' % ctx.flag('zf'))


def _inline_ftol2(ctx):
    """_ftol2 inline, as native::sub_75bed4: st0 truncated into edx:eax."""
    lines = ['const x86::reg64 r = x86::reg64(x86::truncInteger64(double(%s)));' % ctx.st(0)]
    lines += ctx.set_reg('eax', 'x86::reg32(r)') + ctx.set_reg('edx', 'x86::reg32(r >> 32)')
    return ['{'] + ['    ' + l for l in lines] + ['}'] + ctx.fpop()


# Helpers whose calls are emitted inline (Program.inline_calls names them).
INLINE = {'ftol2': _inline_ftol2}


def cg_call(ctx, destination):
    if ctx.fn.branch[ctx.index][0] == 'inline':
        return INLINE[ctx.fn.program.inline_calls[destination.imm]](ctx)
    # The return address slot is not written: callees take their arguments relative to esp.
    if destination.type == X.X86_OP_IMM:
        ctx.boundary('call')
        call = ['%s(app, cpu);' % ctx.function_name(destination.imm)]
        if destination.imm not in ctx.fn.program.pure:
            call.append('if (cpu.terminate) return;')
        if destination.imm == ctx.fn.program.setjmp:
            # setjmp(buffer): the guest jmp_buf is the argument above the return address. A longjmp to it comes back
            # here with the registers in cpu.
            call = ['if (setjmp(rc::jumpBuffer(app, cpu, rc::ld32(%s + 4))) == 0)' % ctx.reg('esp'),
                    '    %s(app, cpu);' % ctx.function_name(destination.imm), 'if (cpu.terminate) return;']
        if not ctx.record and any(x.edge == 'skip' for x in ctx.fn.exits[ctx.index]):
            return _skip_call(ctx, destination.imm, call)
        lines = ctx.set_reg('esp', '%s - 4' % ctx.reg('esp'))
        if ctx.ins.address in getattr(ctx.fn.program, 'return_address_sites', ()):
            # this call's return address in its slot, for a native callee that tells its callers apart
            lines.append('rc::st32(%s, 0x%xu);' % (ctx.reg('esp'), ctx.ins.address + ctx.ins.size))
        return lines + ctx.exit_sync('call') + call + ctx.after_call()
    ctx.boundary('dyncall')
    target = ['cpu.ip = %s;' % ctx.val(destination, 32)]
    call = ['app->dynamic_call(cpu.ip, cpu);', 'if (cpu.terminate) return;']
    return target + ctx.set_reg('esp', '%s - 4' % ctx.reg('esp')) + ctx.exit_sync('call') + call + ctx.after_call()


def _skip_call(ctx, target, call):
    """Call whose callee may also return from this function."""
    (skip,) = ctx.fn.program.stack[target].skips
    lines = ctx.set_reg('esp', '%s - 4' % ctx.reg('esp'))
    lines.append('rc::st32(%s, 0x%xu);' % (ctx.reg('esp'), ctx.ins.address + ctx.ins.size))     # the callee pops it
    lines += ctx.exit_sync('call')
    leave = ctx.fn.store_lines(ctx.index, 'skip') + ['return;']
    lines += ['{', '    const x86::reg32 skip_esp = %s + %d;' % (ctx.reg('esp'), skip)] + ['    ' + l for l in call]
    lines += ['    if (cpu.esp == skip_esp)', '    {'] + ['        ' + l for l in leave] + ['    }', '}']
    return lines + ctx.after_call()


def cg_ret(ctx, *value):
    pop = 4 + (value[0].imm if value else 0)
    return ctx.set_reg('esp', '%s + %d' % (ctx.reg('esp'), pop)) + ctx.exit_sync('exit') + ['return;']
