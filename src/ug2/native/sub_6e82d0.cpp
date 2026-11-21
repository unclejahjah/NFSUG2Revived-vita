// Native sub_6e82d0: samples a quantised animation at a time (thiscall, ret 12: time, output, flag). Locates the key block, updates the two float buffers and writes the blend to the output.

#include <native/anim.h>
#include <native/common.h>
#include <native/fast.h>

namespace ug2
{

void sub_6e82d0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_6e78f0(win32::WinApplication* app, x86::CPU& cpu);
void sub_6e7a80(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

using x86::reg32;

signed char s_on;

bool equalTo(x86::CPU& cpu, Real a, Real b)
{
    cpu.fpu.compare(double(a), double(b));
    return __builtin_parity((cpu.fpu.status.word >> 8) & 0x44) != 0;
}

reg32 blockSize(reg32 shift, reg32 tracks)
{
    return ((((1u << (shift & 31)) + 1) * tracks + 1) & ~1u);
}

// Row value times scale plus offset of one track.
Real quantised(reg32 table, reg32 track, reg32 value, reg32 scale, reg32 offset)
{
    const reg32 entry = table + track * 0x10;
    Real v = Real(int32_t(value));
    v *= ld(entry + scale);
    v += ld(entry + offset);
    return v;
}

// Blend of current and next buffers into output.
template <class Fill>
void blend(reg32 self, reg32 obj, reg32 index, reg32 output, float alpha, Fill fill)
{
    for (reg32 j = 0;;)
    {
        fill(j);
        const reg32 now = rc::ld32(self + 0x18), next = rc::ld32(self + 0x24);
        Real v = ld(next + j * 4);
        v -= ld(now + j * 4);
        ++j;
        v *= Real(alpha);
        v += ld(now + (j - 1) * 4);
        rc::stf32(output + reg32(rc::ld16(index + (j - 1) * 2)) * 4, float(v));
        if (!(int32_t(j) < int32_t(rc::ld16(obj + 0xe))))
            break;
    }
}

bool call(win32::WinApplication* app, x86::CPU& cpu, void (*function)(win32::WinApplication*, x86::CPU&), reg32 below, reg32 object,
          std::initializer_list<reg32> args)
{
    reg32 at = below + 4;
    for (reg32 a : args)
    {
        rc::st32(at, a);
        at += 4;
    }
    cpu.ecx = object;
    cpu.esp = below;
    function(app, cpu);
    return !cpu.terminate;
}

void body(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!fast::enabled(s_on, "anim_quant_native")) [[unlikely]]
    {
        sub_6e82d0_guest(app, cpu);
        return;
    }
    const reg32 entryEsp = cpu.esp, self = cpu.ecx, below = entryEsp - 64;
    const reg32 timeBits = argument(cpu, 0), output = argument(cpu, 1), flag = argument(cpu, 2);
    const reg32 savedEsi = cpu.esi;
    cpu.esi = self;
    if (rc::ld32(self + 0x18) == 0 && !call(app, cpu, sub_6e78f0, below, self, {}))
        return;
    if (flag != 0)
    {
        if (!call(app, cpu, sub_6e7a80, below, self, {timeBits, output, flag}))
            return;
        cpu.esi = savedEsi;
        cpu.esp = entryEsp + 16;
        return;
    }
    float time;
    memcpy(&time, &timeBits, 4);
    if (rc::ld32(self + 0x28) != 0)
    {
        rc::st32(self + 0x10, 0xffffffff);
        rc::st32(self + 0x1c, 0xffffffff);
        rc::st32(self + 0x28, 0);
    }
    reg32 obj = rc::ld32(self + 0xc);
    const int32_t key = int32_t(anim::timeIndex(cpu, time));
    reg32 table = rc::ld32(obj + 8);
    int32_t k;
    if (table == 0)
    {
        if (key < 0)
            k = 0;
        else
        {
            const int32_t count = rc::ld16(obj + 0xc);
            k = key < count ? key : count - 1;
        }
    }
    else if (key < int32_t(rc::ld16(table)))
        k = 0;
    else
    {
        const int32_t current = int32_t(rc::ld32(self + 0x10));
        int32_t c = current < 1 ? 0 : current - 1;
        reg32 p = table + reg32(c) * 2;
        if (int32_t(rc::ld16(p)) > key)
        {
            if (c > 0)
            {
                for (;;)
                {
                    if (int32_t(rc::ld16(p)) <= key)
                        break;
                    --c;
                    p -= 2;
                    if (!(c > 0))
                        break;
                }
            }
        }
        else
        {
            const int32_t last = int32_t(rc::ld16(obj + 0xc)) - 2;
            if (c < last)
            {
                p += 2;
                for (;;)
                {
                    if (int32_t(rc::ld16(p)) > key)
                        break;
                    ++c;
                    p += 2;
                    if (!(c < last))
                        break;
                }
            }
        }
        k = c + 1;
    }
    const reg32 shift = rc::ld8(obj + 0x10);
    const reg32 mask = 0x7fffffffu >> ((31 - shift) & 31);
    const int32_t high = k >> (shift & 31);
    const int32_t low = int32_t(reg32(k) & mask);
    const int32_t current = int32_t(rc::ld32(self + 0x10));
    const int32_t currentHigh = current >> (shift & 31);
    const reg32 tracks = rc::ld16(obj + 0xe);
    const reg32 base = obj + blockSize(shift, tracks) * reg32(high) + tracks * 12 + 0x14;
    const bool backwards = k < current && rc::ld8(0x811e32) == 0;
    const reg32 nextKey = rc::ld32(self + 0x1c);
    bool process = true;
    int32_t currentLow = 0;
    if (reg32(k) == nextKey)
    {
        const reg32 now = rc::ld32(self + 0x18), next = rc::ld32(self + 0x24);
        rc::st32(self + 0x18, next);
        rc::st32(self + 0x24, now);
        rc::st32(self + 0x10, nextKey);
        rc::st32(self + 0x1c, 0xffffffff);
        process = false;
    }
    else if (current != -1 && high == currentHigh && low != 0 && !backwards)
        currentLow = int32_t(reg32(current) & mask);
    else
    {
        if (int32_t(tracks) > 0)
        {
            for (reg32 j = 0;;)
            {
                const Real v = quantised(rc::ld32(self + 0x2c), j, rc::ld16(base + j * 2), 4, 0);
                rc::stf32(rc::ld32(self + 0x18) + j * 4, float(v));
                ++j;
                if (!(int32_t(j) < int32_t(rc::ld16(obj + 0xe))))
                    break;
            }
        }
    }
    if (process)
    {
        if (currentLow < low)
        {
            reg32 now = rc::ld16(obj + 0xe);
            reg32 data = base + reg32(currentLow + 2) * now;
            reg32 left = reg32(low - currentLow);
            do
            {
                if (int32_t(now) > 0)
                {
                    for (reg32 j = 0;;)
                    {
                        Real v = quantised(rc::ld32(self + 0x2c), j, rc::ld8(data + j), 0xc, 8);
                        const reg32 slot = rc::ld32(self + 0x18) + j * 4;
                        v += ld(slot);
                        rc::stf32(slot, float(v));
                        ++j;
                        now = rc::ld16(obj + 0xe);
                        if (!(int32_t(j) < int32_t(now)))
                            break;
                    }
                }
                data += tracks;
                --left;
            } while (left != 0);
        }
        else if (currentLow > low)
        {
            reg32 now = rc::ld16(obj + 0xe);
            reg32 data = base + reg32(currentLow + 1) * now;
            const int32_t top = currentLow - 1;
            if (!(top < low))
            {
                reg32 left = reg32(top - low + 1);
                do
                {
                    if (int32_t(now) > 0)
                    {
                        for (reg32 j = 0;;)
                        {
                            Real v = quantised(rc::ld32(self + 0x2c), j, rc::ld8(data + j), 0xc, 8);
                            const reg32 slot = rc::ld32(self + 0x18) + j * 4;
                            Real r = ld(slot);
                            r -= v;
                            rc::stf32(slot, float(r));
                            ++j;
                            now = rc::ld16(obj + 0xe);
                            if (!(int32_t(j) < int32_t(now)))
                                break;
                        }
                    }
                    data -= tracks;
                    --left;
                } while (left != 0);
            }
        }
    }
    rc::st32(self + 0x10, reg32(k));
    table = rc::ld32(obj + 8);
    const reg32 afterKey = reg32(k) + 1;
    float alpha = 1.0f;
    bool blended;
    const Real a = Real(time);
    if (table == 0)
    {
        const Real here = Real(key);
        blended = !equalTo(cpu, a, here);
        if (blended)
        {
            Real t = a;
            t -= here;
            alpha = float(t);
        }
    }
    else if (k == 0)
    {
        blended = !equalTo(cpu, a, ld(0x7d7d3c));
        if (blended)
        {
            Real t = a;
            t /= Real(int32_t(rc::ld16(table)));
            alpha = float(t);
        }
    }
    else
    {
        const Real before = Real(int32_t(rc::ld16(table + reg32(k) * 2 - 2)));
        blended = !equalTo(cpu, a, before);
        if (blended)
        {
            Real t = a;
            t -= before;
            Real span = Real(int32_t(rc::ld16(table + reg32(k) * 2)));
            span -= before;
            t /= span;
            alpha = float(t);
        }
    }
    reg32 index = rc::ld32(obj + 4);
    if (!blended || k >= int32_t(rc::ld16(obj + 0xc)) - 1)
    {
        if (rc::ld16(obj + 0xe) != 0)
        {
            for (reg32 j = 0;;)
            {
                rc::st32(output + reg32(rc::ld16(index + j * 2)) * 4, rc::ld32(rc::ld32(self + 0x18) + j * 4));
                ++j;
                if (!(int32_t(j) < int32_t(rc::ld16(obj + 0xe))))
                    break;
            }
        }
    }
    else
    {
        const reg32 tracks2 = rc::ld16(obj + 0xe);
        const int32_t high2 = int32_t(afterKey) >> (shift & 31);
        const reg32 base2 = obj + blockSize(rc::ld8(obj + 0x10), tracks2) * reg32(high2) + tracks2 * 12 + 0x14;
        if (afterKey == rc::ld32(self + 0x1c))
        {
            if (int32_t(tracks2) > 0)
                blend(self, obj, index, output, alpha, [](reg32) {});
        }
        else
        {
            if (int32_t(tracks2) > 0)
            {
                if (high2 != high)
                {
                    blend(self, obj, index, output, alpha, [&](reg32 j) {
                        const Real v = quantised(rc::ld32(self + 0x2c), j, rc::ld16(base2 + j * 2), 4, 0);
                        rc::stf32(rc::ld32(self + 0x24) + j * 4, float(v));
                    });
                }
                else
                {
                    const reg32 data2 = base2 + reg32(low + 2) * tracks2;
                    blend(self, obj, index, output, alpha, [&](reg32 j) {
                        Real v = quantised(rc::ld32(self + 0x2c), j, rc::ld8(data2 + j), 0xc, 8);
                        v += ld(rc::ld32(self + 0x18) + j * 4);
                        rc::stf32(rc::ld32(self + 0x24) + j * 4, float(v));
                    });
                }
            }
            rc::st32(self + 0x1c, afterKey);
        }
    }
    const reg32 extra = rc::ld8(obj + 0x11);
    if (extra != 0)
    {
        const reg32 tracks3 = rc::ld16(obj + 0xe), count = rc::ld16(obj + 0xc);
        const reg32 s = rc::ld8(obj + 0x10);
        reg32 base3 = obj + blockSize(s, tracks3) * (count >> (s & 31)) + tracks3 * 12 + 0x14;
        const reg32 rest = (0x7fffffffu >> ((31 - s) & 31)) & count;
        reg32 indexes = base3;
        if (rest != 0)
            indexes = ((rest + 1) * tracks3 + indexes + 1) & ~1u;
        if (rc::ld16(obj + 0xe) == 0)
            indexes = (indexes + 1) & ~1u;
        if (rest != 0)
            base3 = ((rest + 1) * tracks3 + base3 + 1) & ~1u;
        if (rc::ld16(obj + 0xe) == 0)
            base3 = (base3 + 1) & ~1u;
        reg32 values = (base3 + rc::ld8(obj + 0x11) * 2 + 3) & ~3u;
        for (reg32 i = 0; int32_t(i) < int32_t(rc::ld8(obj + 0x11)); ++i)
        {
            rc::st32(output + reg32(rc::ld16(indexes)) * 4, rc::ld32(values));
            indexes += 2;
            values += 4;
        }
    }
    cpu.eax = (cpu.eax & ~0xffu) | 1;
    cpu.esi = savedEsi;
    cpu.esp = entryEsp + 16;
}

}

void sub_6e82d0(win32::WinApplication* app, x86::CPU& cpu)
{
    body(app, cpu); // not shadow-checked: its callees update animation state, a second run would repeat that
}

}}
