// Native sub_6e9780: samples a quantised vec3 animation at a time (thiscall, ret 12: time, output, flag). Same scheme as sub_6e82d0 with three lanes per track; sub_6e88f0 inlined.

#include <native/anim.h>
#include <native/common.h>
#include <native/fast.h>

namespace ug2
{

void sub_6e9780_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_6e89c0(win32::WinApplication* app, x86::CPU& cpu);
void sub_6e8cd0(win32::WinApplication* app, x86::CPU& cpu);

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
    return ((((1u << (shift & 31)) + 1) * tracks * 3 + 1) & ~1u);
}

reg32 blocksBase(reg32 obj, reg32 shift, reg32 tracks, reg32 block)
{
    return obj + blockSize(shift, tracks) * block + tracks * 36 + 0x14;
}

// Value of lane of a track from u16 triples.
Real wide(reg32 table, reg32 track, reg32 base, reg32 lane)
{
    const reg32 entry = table + track * 0x30;
    Real v = Real(int32_t(rc::ld16(base + track * 6 + lane * 2)));
    v *= ld(entry + 0xc + lane * 4);
    v += ld(entry + lane * 4);
    return v;
}

// Value of lane of a track from byte triples.
Real narrow(reg32 table, reg32 track, reg32 data, reg32 lane)
{
    const reg32 entry = table + track * 0x30;
    Real v = Real(int32_t(rc::ld8(data + track * 3 + lane)));
    v *= ld(entry + 0x24 + lane * 4);
    v += ld(entry + 0x18 + lane * 4);
    return v;
}

// Blend of the buffers for one track's three lanes into output.
void blendTrack(reg32 self, reg32 output, reg32 index, reg32 offset, float alpha)
{
    for (reg32 lane = 0; lane < 3; ++lane)
    {
        const reg32 next = rc::ld32(self + 0x24), now = rc::ld32(self + 0x18);
        Real v = ld(next + offset + lane * 4);
        v -= ld(now + offset + lane * 4);
        v *= Real(alpha);
        v += ld(now + offset + lane * 4);
        rc::stf32(output + reg32(rc::ld16(index)) * 4 + lane * 4, float(v));
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

// sub_6e88f0: where the extra index list starts.
reg32 extraStart(reg32 obj)
{
    const reg32 tracks = rc::ld16(obj + 0xe), count = rc::ld16(obj + 0xc), shift = rc::ld8(obj + 0x10);
    reg32 at = blocksBase(obj, shift, tracks, count >> (shift & 31));
    const reg32 rest = (0x7fffffffu >> ((31 - shift) & 31)) & count;
    if (int32_t(rest) > 0)
    {
        const reg32 rows = (rest + 1) * tracks;
        at = (rows + at + rows * 2 + 1) & ~1u;
    }
    if (rc::ld16(obj + 0xe) == 0)
        at = (at + 1) & ~1u;
    return at;
}

void body(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!fast::enabled(s_on, "anim_quant3_native")) [[unlikely]]
    {
        sub_6e9780_guest(app, cpu);
        return;
    }
    const reg32 entryEsp = cpu.esp, self = cpu.ecx, below = entryEsp - 80;
    const reg32 timeBits = argument(cpu, 0), output = argument(cpu, 1), flag = argument(cpu, 2);
    const reg32 savedEsi = cpu.esi;
    cpu.esi = self;
    if (rc::ld32(self + 0x18) == 0 && !call(app, cpu, sub_6e89c0, below, self, {}))
        return;
    if (flag != 0)
    {
        if (!call(app, cpu, sub_6e8cd0, below, self, {timeBits, output, flag}))
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
    const reg32 obj = rc::ld32(self + 0xc);
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
    const reg32 base = blocksBase(obj, shift, tracks, reg32(high));
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
    else if (int32_t(tracks) > 0)
    {
        for (reg32 j = 0;;)
        {
            const reg32 slot = rc::ld32(self + 0x18) + j * 0x10;
            for (reg32 lane = 0; lane < 3; ++lane)
                rc::stf32(slot + lane * 4, float(wide(rc::ld32(self + 0x2c), j, base, lane)));
            ++j;
            if (!(int32_t(j) < int32_t(rc::ld16(obj + 0xe))))
                break;
        }
    }
    if (process)
    {
        const bool forward = currentLow < low;
        if (forward || currentLow > low)
        {
            reg32 now = rc::ld16(obj + 0xe);
            reg32 data = base + reg32(forward ? currentLow + 2 : currentLow + 1) * now * 3;
            bool run = true;
            reg32 left = 0;
            if (forward)
                left = reg32(low - currentLow);
            else if (currentLow - 1 < low)
                run = false;
            else
                left = reg32(currentLow - 1 - low + 1);
            while (run)
            {
                if (int32_t(now) > 0)
                {
                    for (reg32 j = 0;;)
                    {
                        const reg32 slot = rc::ld32(self + 0x18) + j * 0x10;
                        const reg32 table2 = rc::ld32(self + 0x2c);
                        const Real a = narrow(table2, j, data, 0), b = narrow(table2, j, data, 1);
                        const float c = float(narrow(table2, j, data, 2));
                        if (forward)
                        {
                            Real x = a;
                            x += ld(rc::ld32(self + 0x18) + j * 0x10);
                            rc::stf32(rc::ld32(self + 0x18) + j * 0x10, float(x));
                            Real y = b;
                            y += ld(rc::ld32(self + 0x18) + j * 0x10 + 4);
                            rc::stf32(rc::ld32(self + 0x18) + j * 0x10 + 4, float(y));
                            Real z = Real(c);
                            z += ld(rc::ld32(self + 0x18) + j * 0x10 + 8);
                            rc::stf32(rc::ld32(self + 0x18) + j * 0x10 + 8, float(z));
                        }
                        else
                        {
                            Real x = ld(slot);
                            x -= a;
                            rc::stf32(slot, float(x));
                            Real y = ld(rc::ld32(self + 0x18) + j * 0x10 + 4);
                            y -= b;
                            rc::stf32(rc::ld32(self + 0x18) + j * 0x10 + 4, float(y));
                            Real z = ld(rc::ld32(self + 0x18) + j * 0x10 + 8);
                            z -= Real(c);
                            rc::stf32(rc::ld32(self + 0x18) + j * 0x10 + 8, float(z));
                        }
                        ++j;
                        now = rc::ld16(obj + 0xe);
                        if (!(int32_t(j) < int32_t(now)))
                            break;
                    }
                }
                if (forward)
                    data += tracks * 3;
                else
                    data -= tracks * 3;
                if (--left == 0)
                    break;
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
    const reg32 index = rc::ld32(obj + 4);
    if (!blended || k >= int32_t(rc::ld16(obj + 0xc)) - 1)
    {
        if (rc::ld16(obj + 0xe) != 0)
        {
            for (reg32 j = 0;;)
            {
                const reg32 source = rc::ld32(self + 0x18) + j * 0x10;
                const reg32 destination = output + reg32(rc::ld16(index + j * 2)) * 4;
                rc::st32(destination, rc::ld32(source));
                rc::st32(destination + 4, rc::ld32(source + 4));
                rc::st32(destination + 8, rc::ld32(source + 8));
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
        const reg32 base2 = blocksBase(obj, rc::ld8(obj + 0x10), tracks2, reg32(high2));
        if (afterKey == rc::ld32(self + 0x1c))
        {
            if (int32_t(tracks2) > 0)
            {
                for (reg32 j = 0;;)
                {
                    blendTrack(self, output, index + j * 2, j * 0x10, alpha);
                    ++j;
                    if (!(int32_t(j) < int32_t(rc::ld16(obj + 0xe))))
                        break;
                }
            }
        }
        else
        {
            if (int32_t(tracks2) > 0)
            {
                const reg32 data2 = base2 + reg32(low + 2) * tracks2 * 3;
                const bool sameBlock = high2 == high;
                for (reg32 j = 0;;)
                {
                    const reg32 table2 = rc::ld32(self + 0x2c);
                    const reg32 slot = j * 0x10;
                    if (!sameBlock)
                    {
                        for (reg32 lane = 0; lane < 3; ++lane)
                            rc::stf32(rc::ld32(self + 0x24) + slot + lane * 4, float(wide(table2, j, base2, lane)));
                    }
                    else
                    {
                        const Real x = narrow(table2, j, data2, 0), y = narrow(table2, j, data2, 1);
                        const float z = float(narrow(table2, j, data2, 2));
                        Real r = x;
                        r += ld(rc::ld32(self + 0x18) + slot);
                        rc::stf32(rc::ld32(self + 0x24) + slot, float(r));
                        r = y;
                        r += ld(rc::ld32(self + 0x18) + slot + 4);
                        rc::stf32(rc::ld32(self + 0x24) + slot + 4, float(r));
                        r = Real(z);
                        r += ld(rc::ld32(self + 0x18) + slot + 8);
                        rc::stf32(rc::ld32(self + 0x24) + slot + 8, float(r));
                    }
                    blendTrack(self, output, index + j * 2, slot, alpha);
                    ++j;
                    if (!(int32_t(j) < int32_t(rc::ld16(obj + 0xe))))
                        break;
                }
            }
            rc::st32(self + 0x1c, afterKey);
        }
    }
    const reg32 extra = rc::ld8(obj + 0x11);
    if (extra != 0)
    {
        reg32 indexes = extraStart(obj);
        reg32 values = (extraStart(obj) + extra * 2 + 3) & ~3u;
        for (reg32 i = 0; int32_t(i) < int32_t(rc::ld8(obj + 0x11)); ++i)
        {
            const reg32 destination = output + reg32(rc::ld16(indexes)) * 4;
            rc::st32(destination, rc::ld32(values));
            rc::st32(destination + 4, rc::ld32(values + 4));
            rc::st32(destination + 8, rc::ld32(values + 8));
            indexes += 2;
            values += 12;
        }
    }
    cpu.eax = (cpu.eax & ~0xffu) | 1;
    cpu.esi = savedEsi;
    cpu.esp = entryEsp + 16;
}

}

void sub_6e9780(win32::WinApplication* app, x86::CPU& cpu)
{
    body(app, cpu); // not shadow-checked: its callees update animation state, a second run would repeat that
}

}}
