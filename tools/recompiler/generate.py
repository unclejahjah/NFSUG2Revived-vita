"""Generates the C++ recompilation of a game executable into one directory.

Usage: generate.py [--game ug2] --binaries <dir> --out <dir> [--analysis <file.json>]

<dir> must hold the executable named by games/<game>.py. Its SHA-256 is compared with the one the port was tested
with; another executable gets a warning. The routines of the audio banks are translated too when the game has them (bankcode.py).

Files in the output directory are replaced only when their content changes; generated files that are no longer
produced are deleted, other files are left alone.
"""

import argparse
import collections
import filecmp
import hashlib
import importlib
import json
import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'disasm'))
import bootstrap  # noqa: E402
bootstrap.ensure_capstone()
import capstone  # noqa: E402
import bankcode  # noqa: E402
from disasm import analysis, codegen, discover, fpustack, frames, image as pe_image, writer  # noqa: E402

GAME = None                             # the games/<name>.py module chosen with --game


# Names of the generated files: <module>.cpp/.h, <module>.<bucket>.cpp, the layout header and the bank routines.
def generated_file():
    m = re.escape(GAME.MODULE)
    return re.compile(r'^%s(\.[0-9a-f]+)?\.(cpp|h)$|^%s_layout\.h$|^%s_bank\.cpp$' % (m, m, m))


class GameModule:
    """The executable as the analyses and the writer see it."""
    def __init__(self, image, subroutines):
        self.application_name = GAME.MODULE
        self.image = image
        self.entry_point = image.entry_point
        self.exported_symbols = []
        self.subroutines = subroutines


MODIFIED_WARNING = """
WARNING: %(path)s is not the executable this port has been tested with.
  found    SHA-256 %(found)s
  tested   SHA-256 %(expected)s

The port has only been tested against that one executable. With another, the build may fail, or the game may not
start, may crash, or may run much slower. Changes inside functions the port replaces with its own code are ignored.
"""


# Warns about an untested executable, and asks when someone can answer.
def find_binary(directory):
    """The game file in directory, whatever its letter case (the game folder of a PC install), or None."""
    if os.path.isdir(directory):
        for name in os.listdir(directory):
            if name.lower() == GAME.EXE_NAME.lower():
                return os.path.join(directory, name)
    return None


def check_binary(directory):
    path = find_binary(directory) or os.path.join(directory, GAME.EXE_NAME)
    if not os.path.isfile(path):
        sys.exit('missing %s' % path)
    with open(path, 'rb') as f:
        sha256 = hashlib.sha256(f.read()).hexdigest()
    if sha256 != GAME.EXE_SHA256:
        print(MODIFIED_WARNING % {'path': path, 'found': sha256, 'expected': GAME.EXE_SHA256}, flush=True)
        if sys.stdin is not None and sys.stdin.isatty():
            try:
                answer = input('Continue with this executable anyway? [y/N] ')
            except EOFError:
                answer = ''
            if answer.strip().lower() not in ('y', 'yes'):
                sys.exit('stopped: nothing was generated')
        else:
            print('Continuing with it: there is no terminal to ask on.', flush=True)
    return pe_image.Image(path, getattr(GAME, 'RELOCATION_SECTION', None), getattr(GAME, 'REBASE', None))


def apply_patches(image):
    for address, original, patched in GAME.PATCHES:
        original, patched = bytes.fromhex(original), bytes.fromhex(patched)
        s = image.section_of(address)
        if s is None or len(original) != len(patched):
            sys.exit('patch 0x%x: no section or lengths differ' % address)
        offset = address - s.address
        if s.data[offset:offset + len(original)] != original:
            sys.exit('patch 0x%x: bytes %s, expected %s'
                     % (address, s.data[offset:offset + len(original)].hex(), original.hex()))
        s.data = s.data[:offset] + patched + s.data[offset + len(patched):]


def simd_range(address):
    return next((r for r in GAME.SIMD_GATED if r[0] <= address < r[1]), None)


def write_analysis(path, program, module, discovery):
    functions = []
    for sub in module.subroutines:
        a = sub.get_entry_point()
        info = program.stack.get(a)
        functions.append({'entry': a, 'origin': discovery.origin[a], 'ret_live': program.ret_live[a],
                          'live_in': program.live_in[a], 'preserved': info.preserved if info else 0,
                          'native': a in program.native_overrides, 'instructions': len(sub.instructions),
                          'no_return': a in discovery.no_return, 'address_taken': a in program.address_taken})
    sections = [{'name': s.name, 'address': s.address, 'length': s.virtual_size} for s in module.image.sections]
    tables = [{'address': t.address, 'targets': t.targets, 'bounded': t.bounded} for t in discovery.tables.values()]
    rejected = [{'address': a, 'origin': o, 'reason': r} for a, (o, r) in sorted(discovery.rejected.items())]
    sources = {'code_refs': {a: sorted(s) for a, s in discovery.code_refs.items()},
               'data_pointers': {a: sorted(s) for a, s in discovery.data_pointers.items()},
               'string_pointers': {a: sorted(s) for a, s in discovery.string_pointers.items()}}
    traps = [{'address': a, 'mnemonic': m, 'class': k} for a, m, k in sorted(set(codegen.simd_sites))]
    code_ranges = []
    for a in sorted(discovery.code):
        end = a + discovery.decoded[a].size
        if code_ranges and a <= code_ranges[-1][1]:
            code_ranges[-1][1] = max(code_ranges[-1][1], end)
        else:
            code_ranges.append([a, end])
    mnemonics = collections.Counter(discovery.decoded[a].mnemonic for a in discovery.code)
    with open(path, 'w') as f:
        json.dump({'functions': functions, 'sections': sections, 'tables': tables, 'gaps': discovery.gaps(),
                   'rejected': rejected, 'sources': sources, 'simd_traps': traps,
                   'unsupported': sorted(set(codegen.unhandled_sites)), 'rounds': discovery.rounds,
                   'index_tables': discovery.index_tables, 'code_ranges': code_ranges,
                   'mnemonics': dict(mnemonics), 'no_return': sorted(discovery.no_return)}, f)


def sync_output(stage, out_dir):
    """Copies the files generated in stage whose content changed (unchanged files keep their timestamps, so the build
    recompiles just the changed ones) and deletes generated files of earlier runs; other files are left alone."""
    names = set(os.listdir(stage))
    changed = removed = 0
    for name in sorted(names):
        assert generated_file().match(name), name
        target = os.path.join(out_dir, name)
        if not os.path.isfile(target) or not filecmp.cmp(os.path.join(stage, name), target, shallow=False):
            shutil.copyfile(os.path.join(stage, name), target)
            changed += 1
    for name in os.listdir(out_dir):
        if name not in names and generated_file().match(name) and os.path.isfile(os.path.join(out_dir, name)):
            os.remove(os.path.join(out_dir, name))
            removed += 1
    print('generated %d files into %s (%d changed, %d removed)' % (len(names), out_dir, changed, removed))


def promote_stack_slots(program, promote, separate):
    started = time.time()
    analysis_ = frames.Analysis(program)
    result = analysis_.run(promote, separate)
    reasons = collections.Counter()
    accesses = promoted = saves = slots = functions = loads = separated = 0
    for a, f in result.items():
        if f.reason:
            reasons[f.reason.split(' 0x')[0]] += 1
            continue
        accesses += len(f.accesses)
        separated += len(f.stack_sites)
        if f.slots or f.stack_sites:
            program.functions[a].frame = f
        if f.slots:
            functions += 1
            slots += len(f.slots)
            loads += sum(s.entry_load for s in f.slots.values())
            promoted += len(f.sites)
            saves += sum(1 for s in f.sites.values() if s.save)
    print('stack slots: %d functions, %d slots (%d loaded at entry), %d of %d resolved stack accesses (%d register '
          'saves); %d shared esp changes solved, %d conflicting; %d indirect calls settled by choice (%.0f s)'
          % (functions, slots, loads, promoted, accesses, saves, len(analysis_.known), len(analysis_.conflicts),
             sum(len(v) for v in analysis_.chosen.values()), time.time() - started))
    print('stack slots: %d accesses marked as not aliasing other guest memory' % separated)
    print('stack slots: functions left as they are: %s' % ', '.join('%s %d' % kv for kv in reasons.most_common()))


def report(discovery, module, program):
    """Statistics and every place the generated code traps. Returns the number of problems: unsupported
    instructions in reachable code, SIMD outside the gated ranges and sites breaking the flag convention."""
    image = module.image
    origins = collections.Counter(discovery.origin[s.get_entry_point()] for s in module.subroutines)
    instructions = sum(len(s.instructions) for s in module.subroutines)
    code_bytes = sum(discovery.decoded[a].size for a in discovery.code)
    text = sum(s.virtual_size for s in image.code_sections)
    print('functions: %d (%s); %d no-return, %d address-taken; %d discovery rounds'
          % (len(module.subroutines), ', '.join('%s %d' % kv for kv in sorted(origins.items())),
             len(discovery.no_return), len(program.address_taken), discovery.rounds))
    print('instructions: %d emitted, %d distinct (%d of %d code-section bytes)'
          % (instructions, len(discovery.code), code_bytes, text))
    print('jump tables: %d (%d bounded by their switch), byte index tables: %d'
          % (len(discovery.tables), sum(t.bounded for t in discovery.tables.values()), len(discovery.index_tables)))
    reasons = collections.Counter(discovery.rejected.values())
    print('pointer candidates rejected: %d (%s); text dwords skipped: %d'
          % (len(discovery.rejected), ', '.join('%s/%s %d' % (o, r, n) for (o, r), n in sorted(reasons.items())),
             len(discovery.string_pointers)))
    if image.relocations:
        kinds = discovery.relocated_code_addresses()
        print('relocated code addresses: %d (%s)' % (sum(kinds.values()),
                                                    ', '.join('%s %d' % kv for kv in sorted(kinds.items()))))
    gaps = discovery.gaps()
    print('code-section bytes outside functions and tables (not padding): %d in %d ranges'
          % (sum(b - a for a, b in gaps), len(gaps)))
    mnemonics = collections.Counter(discovery.decoded[a].mnemonic for a in discovery.code)
    print('mnemonics: %d distinct' % len(mnemonics))
    calls = fpustack.audit(program)
    print('indirect calls: %d; import table %d (host, no check); float result inferred %d, none inferred %d '
          '(checked: %s); open %d and no static x87 depth %d (stack in cpu.fpu, no check)'
          % (sum(calls.values()), calls.get(('import', 0), 0), calls.get(('inferred', 1), 0),
             calls.get(('inferred', 0), 0), 'yes' if program.indirect_fpu_checked else 'no',
             sum(n for (k, _), n in calls.items() if k == 'open'),
             sum(n for (k, _), n in calls.items() if k == 'unknown')))
    problems = 0
    if program.abi_flags:
        flags = analysis.C.ALL_FLAGS
        readers = sorted(a for a, m in program.live_in.items() if m & flags)
        producers = sorted(a for a, m in program.ret_live.items() if m & flags)
        contract = program.flag_contract()
        print('flag convention: %d functions read flags at entry, %d return flags; indirect jumps with flags: %d '
              '(%d functions write them back)' % (len(readers), len(producers), len(contract['indirect_tails']),
                                                  len(program.indirect_returners)))
        for kind, sites in contract.items():
            if kind == 'indirect_tails':
                continue
            for a, m in sites:
                print('FLAG CONVENTION BROKEN (%s): 0x%x %s' % (kind, a, analysis.state_names(m)))
                problems += 1
    skipping = sorted(a for a, info in program.stack.items() if info.skips)
    returning = sum(1 for fn in program.functions.values() for i in fn.reachable for x in fn.exits[i]
                    if x.edge == 'skip')
    print('return address pops: %d functions (%s), %d calls return from their caller too'
          % (len(skipping), ' '.join('0x%x' % a for a in skipping[:12]), returning))
    for a, why in program.skip_problems:
        print('RETURN ADDRESS POPPED (%s): 0x%x' % (why, a))
        problems += 1
    traps = collections.defaultdict(set)
    for fn in program.functions.values():
        for i in fn.reachable:
            ins = fn.instructions[i]
            if fn.effects[i].boundary == 'trap':
                traps[getattr(ins, 'reason', None) or ins.mnemonic].add(ins.address)
    simd = set(a for a, _, _ in codegen.simd_sites)
    for name, sites in sorted(traps.items()):
        if sites & simd:
            continue
        sites = sorted(sites)
        print('trap: %-28s %4d site(s): %s' % (name, len(sites), ' '.join('0x%x' % a for a in sites[:12])))
    by_range = collections.Counter()
    for a, m, kind in sorted(set(codegen.simd_sites)):
        r = simd_range(a)
        if r is None:
            print('UNGATED SIMD instruction: 0x%x %s (%s)' % (a, m, kind))
            problems += 1
        else:
            by_range[(r[2], kind)] += 1
    for (where, kind), n in sorted(by_range.items()):
        print('gated SIMD trap: %-6s %5d in %s' % (kind, n, where))
    if codegen.unhandled_sites:
        unhandled = collections.Counter(m for _, m in set(codegen.unhandled_sites))
        print('UNSUPPORTED instructions (trap at run time): %s' % ', '.join('%s x%d' % kv for kv in unhandled.items()))
        problems += len(set(codegen.unhandled_sites))
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--game', default='ug2', help='games/<name>.py: the executable and its tables')
    parser.add_argument('--binaries', required=True, help='directory with the game executable')
    parser.add_argument('--out', required=True, help='output directory for the generated sources')
    parser.add_argument('--analysis', help='also write the analysis summary (JSON) for test tools')
    parser.add_argument('--missing-ok', action='store_true',
                        help='a game DLL that is not in --binaries: write the module without code (not available)')
    parser.add_argument('--memory-x87', action='store_true',
                        help='keep the x87 stack in cpu.fpu instead of C++ locals (for comparisons)')
    parser.add_argument('--memory-registers', action='store_true',
                        help='keep x86 registers in cpu instead of C++ locals (for comparisons)')
    parser.add_argument('--flags-across-calls', action='store_true',
                        help='pass flags through calls and returns as any code may read them (for comparisons)')
    parser.add_argument('--reads-zeroed-registers', action='store_true',
                        help='count xor r, r and sub r, r as reads of r (for comparisons)')
    parser.add_argument('--promote-stack-slots', action='store_true',
                        help='keep stack slots no other code can see in C++ locals (disasm/frames.py)')
    parser.add_argument('--separate-stack', action='store_true',
                        help='tell clang that frame accesses and pushes never alias other guest accesses (disasm/frames.py)')
    parser.add_argument('--fold-rdata', action='store_true',
                        help='loads from .rdata (read-only on the PC) become constants')
    parser.add_argument('--promote-bisect', action='store_true',
                        help='keep slots in locals only in functions from promote_lo to promote_hi (settings file)')
    parser.add_argument('--check-poison', action='store_true',
                        help='with --check-assumptions: promoted slots write a poison pattern to guest memory instead')
    parser.add_argument('--check-assumptions', action='store_true',
                        help='log at run time where the facts behind --promote-stack-slots, --separate-stack and --fold-rdata fail')
    parser.add_argument('--flag-audit', action='store_true',
                        help='check at run time that every flag read from cpu was stored there (logs the reader)')
    parser.add_argument('--float-align', choices=('off', 'stack', 'all'), default='off',
                        help='VFP loads and stores for float operands at 4-aligned addresses: none, esp-relative and '
                             'absolute ones, or also those of other base registers (assumed aligned)')
    parser.add_argument('--float-align-check', action='store_true',
                        help='with --float-align: check those addresses at run time and log misaligned sites')
    parser.add_argument('--fpu-check', action='store_true',
                        help='assert the x87 depth after every call whose depth is known (finds leaking callees)')
    parser.add_argument('--inline-ftol2', action='store_true',
                        help='emit calls to _ftol2 (the game\'s INLINE_HELPERS) inline instead of calling the native')
    parser.add_argument('--pure-natives', action='store_true',
                        help='no cpu.terminate check after calls that reach only the game\'s PURE_NATIVES')
    parser.add_argument('--native-writes', action='store_true',
                        help='callers of a native override take eax, ecx and edx as written where its guest body '
                             'writes them on every path')
    args = parser.parse_args()
    global GAME
    GAME = importlib.import_module('games.' + args.game)
    codegen.fpu.FLOAT_ALIGN = args.float_align
    codegen.fpu.FLOAT_ALIGN_CHECK = args.float_align_check
    started = time.time()
    if args.missing_ok and getattr(GAME, 'LIBRARY', False) and not find_binary(args.binaries):
        with tempfile.TemporaryDirectory(prefix='nfsugen-') as stage:
            writer.write_library_stub(stage, GAME.MODULE, GAME.EXE_NAME, 'not in the game folder')
            sync_output(stage, args.out)
        print('%s: not in %s, so the module has no code' % (GAME.EXE_NAME, args.binaries))
        return
    image = check_binary(args.binaries)
    apply_patches(image)
    print('%s: read, %d sections, %d relocations, %d imports, %d patches'
          % (GAME.EXE_NAME, len(image.sections), len(image.relocations), len(image.imports), len(GAME.PATCHES)))
    os.makedirs(args.out, exist_ok=True)

    cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    cs.detail = True
    discovery = discover.Discovery(image, cs, no_return=GAME.NO_RETURN)
    discovery.discover(extra_entries=GAME.NATIVE_OVERRIDES + getattr(GAME, 'EXTRA_ENTRIES', [])
                       + [a for _, a in image.exports if image.is_code(a)]
                       + ([GAME.SETJMP] if GAME.SETJMP is not None else []))
    # A DLL's exports are called through GetProcAddress's pointers.
    discovery.address_taken |= set(a for _, a in image.exports if a in discovery.entries)
    print('discovery took %.0f s' % (time.time() - started))
    subroutines = [discovery.subroutine(e) for e in sorted(discovery.entries)]
    module = GameModule(image, subroutines)
    program = analysis.Program([module], GAME.THREAD_SEGMENTS, [], {}, GAME.FLAGS_PRESERVED, GAME.NOT_INDIRECT, GAME.NATIVE_OVERRIDES,
                               local_registers=not args.memory_registers, local_x87=not args.memory_x87,
                               setjmp=GAME.SETJMP, longjmp_jumps=GAME.LONGJMP_JUMPS, fpu_check=args.fpu_check,
                               fpu_deltas=GAME.FPU_DELTAS, preempt_loops=True, address_taken=discovery.address_taken,
                               abi_flags=not args.flags_across_calls, jump_targets=GAME.JUMP_TARGETS,
                               flag_audit=args.flag_audit, zero_idioms=not args.reads_zeroed_registers,
                               inline_calls=getattr(GAME, 'INLINE_HELPERS', {}) if args.inline_ftol2 else {},
                               pure_natives=getattr(GAME, 'PURE_NATIVES', ()) if args.pure_natives else (),
                               native_writes=args.native_writes)
    # Calls that write their return address, for a native callee that tells its callers apart (RETURN_ADDRESS_SITES)
    program.return_address_sites = frozenset(getattr(GAME, 'RETURN_ADDRESS_SITES', ()))
    program.analyze()
    program.check_assumptions = args.check_assumptions
    program.fold_rdata = args.fold_rdata
    program.promote_bisect = args.promote_bisect
    program.check_poison = args.check_poison
    if args.promote_stack_slots or args.separate_stack:
        promote_stack_slots(program, args.promote_stack_slots, args.separate_stack)
    # Every direct call and tail call must reach a generated function.
    missing = sorted(set(x.target for fn in program.functions.values() for i in fn.reachable for x in fn.exits[i]
                         if x.kind in ('call', 'tail') and x.target not in program.functions))
    if missing:
        sys.exit('calls to addresses that are no function: %s' % ' '.join('0x%x' % a for a in missing[:20]))
    with tempfile.TemporaryDirectory(prefix='nfsugen-') as stage:
        writer.Writer(module, program, stage, library=getattr(GAME, 'LIBRARY', False)).write()
        if GAME.BANK_LIBRARY_TABLE is not None:
            bankcode.write(args.binaries, image, capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32), stage,
                           GAME.BANK_LIBRARY_TABLE, GAME.BANK_DIR, GAME.MODULE,
                           getattr(GAME, 'BANK_DIRECT_LIBRARY', ()), getattr(GAME, 'BANK_NATIVE_LIBRARY', ()))
        sync_output(stage, args.out)
    if args.analysis:
        write_analysis(args.analysis, program, module, discovery)
    problems = report(discovery, module, program)
    print('generation took %.0f s' % (time.time() - started))
    if problems:
        sys.exit('%d unsupported or ungated instruction(s) or flag convention break(s) in reachable code' % problems)


if __name__ == '__main__':
    main()
