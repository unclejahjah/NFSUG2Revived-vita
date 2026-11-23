// Native sub_61b670: walks entry tables for a hash, kind, id and class; returns the matching entry or 0.

#include <native/common.h>
#include <native/shadow.h>
#include <platform/switches.h>
#include <platform/cached.h>
#include <platform/config.h>

namespace ug2
{

void sub_61b670_guest(win32::WinApplication* app, x86::CPU& cpu);
bool roamPartIndexFind(x86::CPU& cpu);   // free roam's index of the parts by car and slot (src/ug2/carpart_cache.cpp)

namespace native
{
namespace
{
using x86::reg32;

// sub_610020: name in set A.
bool inSetA(reg32 v)
{
    switch (v)
    {
    case 0x3455d6b: case 0x163087: case 0xdb7c: case 0xdc00: case 0xac6d1: case 0x150fb80: case 0x290c04d:
    case 0x2ca04df: case 0x72b2e513: case 0x3b8c48a: case 0x4fd0522: case 0x545aec60: case 0x7502da84:
    case 0x79aebfec: case 0xbe48375e:
        return true;
    }
    return false;
}

// sub_6100a0: name in set B.
bool inSetB(reg32 v)
{
    switch (v)
    {
    case 0x30236ee: case 0xacc9: case 0x453: case 0x6e7: case 0xa86e: case 0xdc01: case 0x293fad7:
    case 0x4b10070a: case 0x9d2eff6: case 0x2d7c9b5d: case 0x35165819: case 0xcf4458f8: case 0xf77c13eb:
        return true;
    }
    return false;
}

// sub_43db50: string hash.
reg32 hashString(reg32 at)
{
    reg32 h = 0xffffffffu;
    for (reg32 c; (c = rc::ld8(at)) != 0; ++at)
        h = h * 33 + c;
    return h;
}

// sub_488c50(key, base, count, 7, 14), with its ecx and edx.
reg32 search(reg32 key, reg32 base, reg32 count, reg32& ecx, reg32& edx)
{
    if (base == 0 || int32_t(count) <= 0)
        return 0;
    reg32 lo = 0, hi = count - 1;
    for (;;)
    {
        const reg32 sum = lo + hi;
        const reg32 sign = int32_t(sum) < 0 ? 0xffffffffu : 0;
        const reg32 mid = reg32(int32_t(sum - sign) >> 1);
        edx = (sign & 0xffffff00u) | key;
        reg32 p = mid * 14 + base;
        ecx = rc::ld8(p + 7);
        if (key < ecx)
            hi = mid - 1;
        else if (key == ecx)
        {
            if (int32_t(mid) <= 0)
                return p;
            ecx = p - 14 + 7;
            for (reg32 left = mid; rc::ld8(ecx) == key;)
            {
                p -= 14;
                ecx -= 14;
                if (int32_t(--left) <= 0)
                    break;
            }
            return p;
        }
        else
            lo = mid + 1;
        if (int32_t(lo) > int32_t(hi))
            return 0;
    }
}

// Entry hash-list lookup; ecx as the guest leaves it.
reg32 findEntry(reg32 entry, reg32 hash, reg32& ecx)
{
    const reg32 cx = rc::ld16(entry + 0xa);
    ecx = (entry & 0xffff0000u) | cx;
    if (cx == 0xffff)
        return 0;
    const reg32 table = rc::ld32(0x8a1cc8);
    const reg32 list = rc::ld32(table + 0x14) + cx * 2;
    const int32_t count = int16_t(rc::ld16(list));
    ecx = list;
    if (count <= 0)
        return 0;
    const reg32 entries = rc::ld32(table + 0x1c);
    reg32 at = list + 2;
    for (int32_t i = 0; i < count; ++i, at += 2)
    {
        const reg32 e = entries + reg32(int32_t(int16_t(rc::ld16(at)))) * 8;
        if (hash == 0 || rc::ld32(e) == hash)
        {
            ecx = at;
            return e == 0 ? 0 : rc::ld32(e + 4);
        }
    }
    ecx = at;
    return 0;
}

void body(win32::WinApplication*, x86::CPU& cpu)
{
    const reg32 E = cpu.esp;
    const reg32 name = argument(cpu, 0), kind = argument(cpu, 1), id = argument(cpu, 2), resume = argument(cpu, 3),
                klass = argument(cpu, 4);
    const reg32 self = cpu.ecx;
    reg32 ecx = self, edx = cpu.edx, result = 0;
    const reg32 keyString = inSetA(name)   ? 0x7a8550
                            : inSetB(name) ? 0x7a8544
                            : name == 0x219c94a || name == 0x6126dd4d || name == 0xcf704771 ? 0x7a8538
                                                                                         : 0;
    reg32 node = rc::ld32(self);
    if (node != self)
    {
        reg32 index = 0xff;
        if (resume == 0)
        {
            const reg32 n = rc::ld32(0x8a1cbc);
            if (n != 0)
            {
                const reg32 names = rc::ld32(0x8a1cb8);
                for (reg32 i = 0; i < n; ++i)
                    if (rc::ld32(names + 4 * i) == name)
                    {
                        index = i & 0xff;
                        break;
                    }
            }
        }
        reg32 keyHash = 0, thirdHash = 0;
        bool haveKey = false, haveThird = false;
        do
        {
            edx = rc::ld32(node + 0x38);
            ecx = rc::ld32(node + 0x34);
            const reg32 end = edx * 14 + ecx;
            reg32 entry;
            if (resume != 0)
            {
                if (resume < ecx || resume >= end)
                    continue;
                entry = resume + 14;
            }
            else
            {
                entry = search(index, ecx, edx, ecx, edx);
                if (entry == 0)
                    continue;
            }
            if (entry >= end)
                continue;
            const reg32 names = rc::ld32(0x8a1cb8);
            do
            {
                ecx = rc::ld8(entry + 7);
                edx = names;
                if (rc::ld32(names + ecx * 4) != name || reg32(int32_t(int8_t(rc::ld8(entry + 4)))) != kind)
                    continue;
                if (id != 0)
                {
                    edx = rc::ld16(entry);
                    ecx = (reg32(rc::ld16(entry + 2)) << 16) | edx;
                    if (ecx != id)
                        continue;
                }
                if (keyString != 0)
                {
                    if (!haveKey)
                    {
                        keyHash = hashString(keyString);
                        haveKey = true;
                    }
                    reg32 unused;
                    if (findEntry(entry, keyHash, unused) != 0)
                    {
                        if (!haveThird)
                        {
                            thirdHash = keyString == 0x7a8538 ? keyHash : hashString(0x7a8538);
                            haveThird = true;
                        }
                        edx = findEntry(entry, thirdHash, ecx);
                        continue;
                    }
                }
                ecx = entry;
                edx = reg32(int8_t(rc::ld8(entry + 5) >> 5));
                if (edx == klass || klass == 0xffffffffu)
                {
                    result = entry;
                    goto done;
                }
            } while ((entry += 14) < end);
        } while ((node = rc::ld32(node)) != self);
    }
done:
    cpu.eax = result;
    cpu.ecx = ecx;
    cpu.edx = edx;
    cpu.esp = E + 24;
}

void declare(x86::CPU&, shadow::Spans&) {}

const shadow::Spec spec = {"sub_61b670", shadow::Eax | shadow::Ecx | shadow::Edx | shadow::Ebx | shadow::Ebp | shadow::Esi | shadow::Edi, 0, false, false};

}

void sub_61b670(win32::WinApplication* app, x86::CPU& cpu)
{
    if (roamPartIndexFind(cpu))
        return;
    if (!NFSU_NATIVE("sub_61b670_native"))
    {
        sub_61b670_guest(app, cpu);
        return;
    }
    shadow::call(spec, app, cpu, body, sub_61b670_guest, declare);
}

}}
