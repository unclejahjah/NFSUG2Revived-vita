// Native versions of the library functions the translated bank routines call most (BANK_NATIVE_LIBRARY): the same
// results, stores and FPU status as their translations. bank_lib_check runs both for every call and compares.

#include "banknative.h"

#include <lib/memmap.h>
#include <platform/config.h>
#include <platform/switches.h>
#include <platform/log.h>
#include <recomp.h>
#include <x86.h>

#include <cstring>

namespace ug2
{

void sub_71feef(win32::WinApplication* app, x86::CPU& cpu);
void sub_71fefb(win32::WinApplication* app, x86::CPU& cpu);
void sub_720340(win32::WinApplication* app, x86::CPU& cpu);
void sub_7203bd(win32::WinApplication* app, x86::CPU& cpu);
void sub_7203eb(win32::WinApplication* app, x86::CPU& cpu);
void sub_7204f3(win32::WinApplication* app, x86::CPU& cpu);
void sub_720684(win32::WinApplication* app, x86::CPU& cpu);
void sub_720738(win32::WinApplication* app, x86::CPU& cpu);
void sub_720767(win32::WinApplication* app, x86::CPU& cpu);
void sub_720784(win32::WinApplication* app, x86::CPU& cpu);
void sub_7207bf(win32::WinApplication* app, x86::CPU& cpu);
void sub_720f32(win32::WinApplication* app, x86::CPU& cpu);
void sub_720fd1(win32::WinApplication* app, x86::CPU& cpu);
void sub_720ff8(win32::WinApplication* app, x86::CPU& cpu);

namespace bankcode { namespace lib
{

namespace
{

using x86::sreg32;
using x86::Float;
using x86::reg8;

// esp is the call's (the return slot); the argument is above it. Writes below esp + 4 are left out: dead stack.
using Body = reg32 (*)(x86::CPU& cpu, reg32 eax, reg32& ecx, reg32& edx, reg32 esp);
using Guest = void (*)(win32::WinApplication* app, x86::CPU& cpu);

struct Spec
{
    reg32       address;
    Body        body;
    Guest       guest;
    reg32       block;      // bytes from the argument the function may write (bank_lib_check compares them)
};

NFSU_DEV_VARC(bool, s_check, false, false);
win32::WinApplication* s_app = nullptr;

inline reg32 argument(reg32 esp)
{
    return rc::ld32(esp + 4);
}

inline reg32 fist(x86::CPU& cpu, Float value)
{
    return reg32(x86::fistInteger<sreg32>(Float(x86::roundIntegral(double(value), cpu.fpu.control.rc))));
}

// fild*fimul*fmul of a parameter block, stored to the argument slot and read back as an integer.
reg32 f720767Body(x86::CPU& cpu, reg32, reg32&, reg32&, reg32 esp)
{
    const reg32 p = argument(esp);
    Float value = Float(sreg32(rc::ld32(p + 8)));
    value *= Float(sreg32(rc::ld32(p + 4)));
    value *= Float(Float(rc::ldf32(p)));
    const float stored = float(value);
    rc::stf32(esp + 4, stored);
    return fist(cpu, Float(stored));
}

reg32 f720738Body(x86::CPU& cpu, reg32, reg32& ecx, reg32& edx, reg32 esp)
{
    const reg32 p = argument(esp);
    edx = rc::ld8(p);
    Float value = Float(sreg32(rc::ld32(p + 8)));
    if (sreg32(edx) > 1)
    {
        ecx = p + 0xc;
        edx -= 1;
        do
        {
            value *= Float(sreg32(rc::ld32(ecx)));
            ecx += 4;
        } while (--edx != 0);
    }
    value *= Float(Float(rc::ldf32(p + 4)));
    const float stored = float(value);
    rc::stf32(esp + 4, stored);
    return fist(cpu, Float(stored));
}

// max(p[1] - p[2], p[0]), signed.
reg32 f720ff8Body(x86::CPU&, reg32, reg32& ecx, reg32&, reg32 esp)
{
    const reg32 p = argument(esp);
    const reg32 difference = rc::ld32(p + 4) - rc::ld32(p + 8);
    ecx = rc::ld32(p);
    return sreg32(difference) >= sreg32(ecx) ? difference : ecx;
}

// Entry ecx (1..count byte) of a table, else 0.
reg32 f720684Body(x86::CPU&, reg32, reg32& ecx, reg32& edx, reg32 esp)
{
    const reg32 p = argument(esp);
    ecx = rc::ld32(p + 4);
    if (sreg32(ecx) <= 0)
        return 0;
    edx = rc::ld8(p);
    if (sreg32(ecx) > sreg32(edx))
        return 0;
    return rc::ld32(p + ecx * 4 + 4);
}

// Sum of count values clamped to a maximum (signed).
reg32 f720fd1Body(x86::CPU&, reg32, reg32& ecx, reg32& edx, reg32 esp)
{
    ecx = argument(esp);
    reg32 eax = rc::ld32(ecx + 8);
    reg32 count = rc::ld8(ecx);
    if (sreg32(count) > 1)
    {
        edx = ecx + 0xc;
        count -= 1;
        do
        {
            eax += rc::ld32(edx);
            edx += 4;
        } while (--count != 0);
    }
    ecx = rc::ld32(ecx + 4);
    return sreg32(eax) > sreg32(ecx) ? ecx : eax;
}

// Sum of count values.
reg32 f720784Body(x86::CPU&, reg32, reg32& ecx, reg32& edx, reg32 esp)
{
    ecx = argument(esp);
    edx = rc::ld8(ecx);
    reg32 eax = rc::ld32(ecx + 4);
    if (sreg32(edx) <= 1)
        return eax;
    ecx += 8;
    edx -= 1;
    do
    {
        eax += rc::ld32(ecx);
        ecx += 4;
    } while (--edx != 0);
    return eax;
}

// Signed quotient, 0 for a zero divisor.
reg32 f7207bfBody(x86::CPU&, reg32, reg32& ecx, reg32& edx, reg32 esp)
{
    const reg32 p = argument(esp);
    ecx = rc::ld32(p + 4);
    if (ecx == 0)
        return 0;
    const reg32 dividend = rc::ld32(p);
    const rc::Division v = rc::divideSigned(reg32(sreg32(dividend) >> 31), dividend, ecx);
    edx = v.remainder;
    return v.quotient;
}

// The value (+4) set from the first of count flags that is set, kept when none is.
reg32 f7203bdBody(x86::CPU&, reg32, reg32& ecx, reg32& edx, reg32 esp)
{
    const reg32 p = argument(esp);
    ecx = rc::ld16(p) + p;
    edx = 0;
    if (rc::ld8(p + 2) != 0)
    {
        do
        {
            if (rc::ld32(ecx + 4 * edx) != 0)
            {
                ecx = rc::ld32(p + 4 * edx + 8);
                rc::st32(p + 4, ecx);
                break;
            }
            ++edx;
        } while (sreg32(edx) < sreg32(reg32(rc::ld8(p + 2))));
    }
    return rc::ld32(p + 4);
}

// 1 when any of count dwords is set.
reg32 f7203ebBody(x86::CPU&, reg32, reg32& ecx, reg32& edx, reg32 esp)
{
    ecx = argument(esp);
    const reg32 count = rc::ld8(ecx);
    edx = 0;
    if (sreg32(count) <= 0)
        return 0;
    ecx += 4;
    for (;;)
    {
        if (rc::ld32(ecx) != 0)
            return 1;
        ++edx;
        ecx += 4;
        if (!(sreg32(edx) < sreg32(count)))
            return 0;
    }
}

// Value at +0x10, cleared.
reg32 f71feefBody(x86::CPU&, reg32, reg32& ecx, reg32&, reg32 esp)
{
    ecx = argument(esp);
    const reg32 value = rc::ld32(ecx + 0x10);
    rc::st32(ecx + 0x10, 0);
    return value;
}

reg32 f71fefbBody(x86::CPU&, reg32, reg32&, reg32&, reg32 esp)
{
    return rc::ld32(argument(esp) + 0x14);
}

// Range test of +0x14 with entered/inside bytes at +0x10 and +0x11.
reg32 f720340Body(x86::CPU&, reg32, reg32& ecx, reg32& edx, reg32 esp)
{
    const reg32 p = argument(esp);
    ecx = rc::ld32(p + 0x14);
    edx &= 0xffffff00u;
    if (!(sreg32(ecx) < sreg32(rc::ld32(p))) && !(sreg32(ecx) > sreg32(rc::ld32(p + 4))))
    {
        if (rc::ld8(p + 0x10) != 0)
        {
            rc::st8(p + 0x11, 0);
            return 0;
        }
        rc::st8(p + 0x10, 1);
        rc::st8(p + 0x11, 1);
        return 1;
    }
    if (!(sreg32(ecx) < sreg32(rc::ld32(p + 8))) && !(sreg32(ecx) > sreg32(rc::ld32(p + 0xc))))
        rc::st8(p + 0x10, 0);
    rc::st8(p + 0x11, 0);
    return 0;
}

// Table lookup with linear interpolation, cached on the input (+0xC input, +4 last input, +8 result).
reg32 f7204f3Body(x86::CPU& cpu, reg32 eax, reg32& ecx, reg32& edx, reg32 esp)
{
    const reg32 p = argument(esp);
    const reg32 input = rc::ld32(p + 0xc);
    if (input == rc::ld32(p + 4))
        return rc::ld32(p + 8);
    ecx = rc::ld32(p);
    rc::st32(p + 4, input);
    const reg32 first = rc::ld32(ecx + 4);
    reg32 clamped = rc::ld32(ecx + 8);
    if (sreg32(input) < sreg32(first))
        clamped = first;
    else if (!(sreg32(input) > sreg32(clamped)))
        clamped = input;
    edx = clamped - first;
    const Float one = Float(rc::ldf32(0x784250u));
    const Float scale = Float(rc::ldf32(ecx + 0xcu));
    cpu.fpu.compare(double(scale), double(one));
    const reg8 kind = rc::ld8(ecx);
    reg32 result;
    if (double(scale) == double(one))
    {
        rc::st32(esp + 4, edx);
        if (kind == 2)
            result = reg32(x86::sreg16(rc::ld16(ecx + edx * 2 + 0x10u)));
        else if (kind == 1)
            result = reg32(x86::sreg8(rc::ld8(edx + ecx + 0x10u)));
        else
            result = rc::ld32(ecx + edx * 4 + 0x10u);
        RC_USE(eax);
    }
    else
    {
        Float position = Float(sreg32(edx));
        position *= Float(Float(rc::ldf32(ecx + 0xcu)));
        const float exact = float(position);
        position -= Float(Float(rc::ldf32(0x784260u)));
        const float lowered = float(position);
        const reg32 index = fist(cpu, Float(lowered));
        Float fraction = Float(sreg32(index));
        edx = reg32(rc::ld16(ecx + 2u));
        reg32 next = index + 1;
        fraction = Float(Float(exact)) - Float(fraction);
        if (!(sreg32(next) < sreg32(edx)))
            next = edx - 1;
        edx = (edx & 0xffffff00u) | kind;
        Float low, high;
        if (kind == 2)
        {
            edx = reg32(x86::sreg16(rc::ld16(ecx + index * 2 + 0x10u)));
            low = Float(sreg32(edx));
            high = Float(sreg32(reg32(x86::sreg16(rc::ld16(ecx + next * 2 + 0x10u)))));
        }
        else if (kind == 1)
        {
            edx = reg32(x86::sreg8(rc::ld8(index + ecx + 0x10u)));
            low = Float(sreg32(edx));
            high = Float(sreg32(reg32(x86::sreg8(rc::ld8(next + ecx + 0x10u)))));
        }
        else
        {
            low = Float(sreg32(rc::ld32(ecx + index * 4 + 0x10u)));
            high = Float(sreg32(rc::ld32(ecx + next * 4 + 0x10u)));
        }
        high -= Float(low);
        high *= Float(fraction);
        high += Float(low);
        const float value = float(high);
        rc::stf32(esp + 4, value);
        result = fist(cpu, Float(value));
    }
    rc::st32(p + 8, result);
    return result;
}

// Smoothed value toward an integer target (+0x18), with rate state at +4 .. +0x14.
reg32 f720f32Body(x86::CPU& cpu, reg32 eax, reg32& ecx, reg32& edx, reg32 esp)
{
    ecx = argument(esp);
    edx = rc::ld32(ecx + 0x18u);
    rc::st32(esp + 4, edx);
    const Float target = Float(sreg32(edx));
    const Float current = Float(rc::ldf32(ecx));
    if (double(target) == double(current))
        return edx;
    bool update = true;
    if (edx == rc::ld32(ecx + 8u))
    {
        eax = rc::ld32(ecx + 0x10u);
        update = eax != rc::ld32(ecx + 0xcu);
    }
    if (update)
    {
        eax = rc::ld32(ecx + 0x10u);
        rc::st32(ecx + 8u, edx);
        rc::st32(esp + 4, eax);
        rc::st32(ecx + 0xcu, eax);
        if (!(sreg32(eax) > 0))
        {
            rc::stf32(ecx, float(target));
            return edx;
        }
        Float step = target;
        step -= Float(Float(rc::ldf32(ecx)));
        rc::stf32(ecx + 4u, float(step));
        Float rate = Float(step) / Float(Float(sreg32(rc::ld32(esp + 4))));
        rate *= Float(Float(rc::ldf32(0x8b4054u)));
        rate *= Float(Float(rc::ldf32(0x789ec4u)));
        rc::stf32(ecx + 4u, float(rate));
    }
    Float next = Float(sreg32(rc::ld32(ecx + 0x14u)));
    next *= Float(Float(rc::ldf32(ecx + 4u)));
    next += Float(Float(rc::ldf32(ecx)));
    rc::stf32(ecx, float(next));
    const Float rate = Float(rc::ldf32(ecx + 4u));
    cpu.fpu.compare(double(rate), double(Float(rc::ldf32(0x7d7d3cu))));
    const reg8 first = reg8(cpu.fpu.status.word >> 8);
    cpu.fpu.compare(double(next), double(target));
    const reg8 second = reg8(cpu.fpu.status.word >> 8);
    const bool clamp = (first & 1) ? x86::parity8(reg8(second & 5)) : (second & 0x41) == 0;
    if (clamp)
        rc::stf32(ecx, float(target));
    const reg32 bits = rc::ld32(ecx);
    rc::st32(esp + 4, bits);
    return fist(cpu, Float(rc::ldf32(esp + 4)));
}

const Spec s_specs[] = {
    {0x71feef, &f71feefBody, &sub_71feef, 0x14},
    {0x71fefb, &f71fefbBody, &sub_71fefb, 0},
    {0x720340, &f720340Body, &sub_720340, 0x14},
    {0x7203eb, &f7203ebBody, &sub_7203eb, 0},
    {0x7204f3, &f7204f3Body, &sub_7204f3, 0x10},
    {0x720684, &f720684Body, &sub_720684, 0},
    {0x720738, &f720738Body, &sub_720738, 0},
    {0x720767, &f720767Body, &sub_720767, 0},
    {0x720784, &f720784Body, &sub_720784, 0},
    {0x7207bf, &f7207bfBody, &sub_7207bf, 0},
    {0x720f32, &f720f32Body, &sub_720f32, 0x1c},
    {0x720fd1, &f720fd1Body, &sub_720fd1, 0},
    {0x720ff8, &f720ff8Body, &sub_720ff8, 0},
    {0x7203bd, &f7203bdBody, &sub_7203bd, 8},
};
const int SpecCount = int(sizeof(s_specs) / sizeof(s_specs[0]));

const reg32 StackBytes = 16;        // argument slots compared
const reg32 DeadBytes  = 0x40;      // the callee's frame below the return slot, restored but not compared
const reg32 BlockMax   = 0x20;

struct Tally
{
    uint32_t checks = 0;
    uint32_t differing = 0;
};
Tally s_tallies[SpecCount];
uint32_t s_total = 0;
uint32_t s_logged = 0;
const uint32_t SummaryEvery = 200000;
const uint32_t Details = 24;

struct Image
{
    uint8_t stack[StackBytes];
    uint8_t block[BlockMax];
};

void save(Image& image, reg32 esp, reg32 block, reg32 size)
{
    memcpy(image.stack, rc::host(esp + 4), StackBytes);
    if (size)
        memcpy(image.block, rc::host(block), size);
}

void restore(const Image& image, reg32 esp, reg32 block, reg32 size)
{
    memcpy(rc::host(esp + 4), image.stack, StackBytes);
    if (size)
        memcpy(rc::host(block), image.block, size);
}

void report(const Spec& spec, reg32 esp, const char* what, reg32 native, reg32 guest)
{
    if (s_logged >= Details)
        return;
    ++s_logged;
    pf::logInfo("bank_lib_check: 0x%06x (argument 0x%08x) %s native 0x%08x guest 0x%08x", unsigned(spec.address),
                unsigned(argument(esp)), what, unsigned(native), unsigned(guest));
}

// The translation first on a copy of the state, its memory effects undone, then the native body; differences logged.
reg32 checked(int index, win32::WinApplication* app, x86::CPU& cpu, reg32 eax, reg32& ecx, reg32& edx, reg32 esp)
{
    const Spec& spec = s_specs[index];
    const reg32 block = spec.block ? argument(esp) : 0;
    const reg32 size = spec.block && win32::MemMap::isGuestRange(block, spec.block) ? spec.block : 0;
    Image before, guestAfter, nativeAfter;
    uint8_t dead[DeadBytes];
    save(before, esp, block, size);
    memcpy(dead, rc::host(esp + 4 - DeadBytes), DeadBytes);
    x86::CPU guest = cpu;
    guest.eax = eax;
    guest.ecx = ecx;
    guest.edx = edx;
    guest.esp = esp;
    spec.guest(app, guest);
    save(guestAfter, esp, block, size);
    restore(before, esp, block, size);
    memcpy(rc::host(esp + 4 - DeadBytes), dead, DeadBytes);
    reg32 nativeEcx = ecx, nativeEdx = edx;
    const reg32 result = spec.body(cpu, eax, nativeEcx, nativeEdx, esp);
    save(nativeAfter, esp, block, size);
    Tally& tally = s_tallies[index];
    ++tally.checks;
    const uint32_t logged = s_logged;
    if (result != guest.eax)
        report(spec, esp, "eax", result, guest.eax);
    if (nativeEcx != guest.ecx)
        report(spec, esp, "ecx", nativeEcx, guest.ecx);
    if (nativeEdx != guest.edx)
        report(spec, esp, "edx", nativeEdx, guest.edx);
    if (guest.esp != esp + 4)
        report(spec, esp, "esp", esp + 4, guest.esp);
    if (guest.ebx != cpu.ebx || guest.ebp != cpu.ebp || guest.esi != cpu.esi || guest.edi != cpu.edi)
        report(spec, esp, "ebx/ebp/esi/edi", cpu.esi, guest.esi);
    if (guest.flags.eflags != cpu.flags.eflags)
        report(spec, esp, "eflags", cpu.flags.eflags, guest.flags.eflags);
    if (guest.fpu.status.word != cpu.fpu.status.word || guest.fpu.count != cpu.fpu.count)
        report(spec, esp, "fpu status", cpu.fpu.status.word, guest.fpu.status.word);
    for (reg32 i = 0; i < StackBytes; i += 4)
    {
        reg32 n, g;
        memcpy(&n, nativeAfter.stack + i, 4);
        memcpy(&g, guestAfter.stack + i, 4);
        if (n != g)
            report(spec, esp, i ? "argument slot" : "first argument slot", n, g);
    }
    for (reg32 i = 0; i < size; ++i)
        if (nativeAfter.block[i] != guestAfter.block[i])
        {
            report(spec, esp, "block byte", block + i, (reg32(nativeAfter.block[i]) << 8) | guestAfter.block[i]);
            break;
        }
    const bool differs = s_logged != logged || (logged >= Details && (result != guest.eax || nativeEcx != guest.ecx
                         || nativeEdx != guest.edx || memcmp(&nativeAfter, &guestAfter, sizeof(Image)) != 0));
    tally.differing += differs;
    if (++s_total % SummaryEvery == 0)
    {
        char line[640];
        int length = snprintf(line, sizeof(line), "bank_lib_check: %u calls compared:", unsigned(s_total));
        for (int i = 0; i < SpecCount && length < int(sizeof(line)) - 40; ++i)
            length += snprintf(line + length, sizeof(line) - length, " %x %u/%u differ", unsigned(s_specs[i].address),
                               unsigned(s_tallies[i].checks), unsigned(s_tallies[i].differing));
        pf::logInfo("%s", line);
    }
    ecx = nativeEcx;
    edx = nativeEdx;
    return result;
}

template <int Index>
inline reg32 run(x86::CPU& cpu, reg32 eax, reg32& ecx, reg32& edx, reg32 esp)
{
    if (__builtin_expect(s_check, 0))
        return checked(Index, s_app, cpu, eax, ecx, edx, esp);
    return s_specs[Index].body(cpu, eax, ecx, edx, esp);
}

}

void configure(win32::WinApplication* app)
{
    s_app = app;
    NFSU_DEV_SET(s_check, NFSU_DEV_BOOL("bank_lib_check", false));
    if (s_check)
        pf::logInfo("bank_lib_check: %d native library functions compared with their translations", SpecCount);
}

reg32 f71feef(x86::CPU& cpu, reg32* cd, reg32 eax, reg32 esp)
{
    return run<0>(cpu, eax, cd[0], cd[1], esp);
}

reg32 f71fefb(x86::CPU& cpu, reg32* cd, reg32 eax, reg32 esp)
{
    return run<1>(cpu, eax, cd[0], cd[1], esp);
}

reg32 f720340(x86::CPU& cpu, reg32* cd, reg32 eax, reg32 esp)
{
    return run<2>(cpu, eax, cd[0], cd[1], esp);
}

reg32 f7203eb(x86::CPU& cpu, reg32* cd, reg32 eax, reg32 esp)
{
    return run<3>(cpu, eax, cd[0], cd[1], esp);
}

reg32 f7203bd(x86::CPU& cpu, reg32* cd, reg32 eax, reg32 esp)
{
    return run<13>(cpu, eax, cd[0], cd[1], esp);
}

reg32 f7204f3(x86::CPU& cpu, reg32* cd, reg32 eax, reg32 esp)
{
    return run<4>(cpu, eax, cd[0], cd[1], esp);
}

reg32 f720684(x86::CPU& cpu, reg32* cd, reg32 eax, reg32 esp)
{
    return run<5>(cpu, eax, cd[0], cd[1], esp);
}

reg32 f720738(x86::CPU& cpu, reg32* cd, reg32 eax, reg32 esp)
{
    return run<6>(cpu, eax, cd[0], cd[1], esp);
}

reg32 f720767(x86::CPU& cpu, reg32* cd, reg32 eax, reg32 esp)
{
    return run<7>(cpu, eax, cd[0], cd[1], esp);
}

reg32 f720784(x86::CPU& cpu, reg32* cd, reg32 eax, reg32 esp)
{
    return run<8>(cpu, eax, cd[0], cd[1], esp);
}

reg32 f7207bf(x86::CPU& cpu, reg32* cd, reg32 eax, reg32 esp)
{
    return run<9>(cpu, eax, cd[0], cd[1], esp);
}

reg32 f720f32(x86::CPU& cpu, reg32* cd, reg32 eax, reg32 esp)
{
    return run<10>(cpu, eax, cd[0], cd[1], esp);
}

reg32 f720fd1(x86::CPU& cpu, reg32* cd, reg32 eax, reg32 esp)
{
    return run<11>(cpu, eax, cd[0], cd[1], esp);
}

reg32 f720ff8(x86::CPU& cpu, reg32* cd, reg32 eax, reg32 esp)
{
    return run<12>(cpu, eax, cd[0], cd[1], esp);
}

}}}
