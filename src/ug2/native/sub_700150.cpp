// Native sub_700150: the game's movie frame conversion (cdecl: Y, U, V, Y pitch, width, height, out, out pitch, format)
// for format 1, YV12 to 32-bit pixels through the MMX lookup tables. Other formats run the translated original.

#include <native/common.h>
#include <native/shadow.h>
#include <platform/switches.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace ug2
{

void sub_700150_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

const x86::reg32 TableY = 0x008149E0, TableU = 0x008151E0, TableV = 0x008159E0;
const x86::reg32 TableOrder = 0x008B2318;      // channel order of the tables; 0x700059 swaps words 0 and 1 when set

struct Arguments
{
    x86::reg32 y, u, v, yPitch, width, height, out, outPitch;
};

Arguments read(x86::reg32 entry)
{
    return { rc::ld32(entry + 4), rc::ld32(entry + 8), rc::ld32(entry + 12), rc::ld32(entry + 16),
             rc::ld32(entry + 20), rc::ld32(entry + 24), rc::ld32(entry + 28), rc::ld32(entry + 32) };
}

// First written pixel and pixel count (even) once the guest's alignment rules are applied.
void span(const Arguments& a, x86::reg32& start, x86::reg32& pixels)
{
    start = a.out;
    pixels = a.width;
    if (a.out & 7)
    {
        start = a.out + 4;
        pixels = a.width - 1;
    }
    pixels &= ~1u;
}

void swapTables()
{
    for (x86::reg32 at = 0; at < 0x7F8; at += 8)
    {
        for (x86::reg32 table : { TableY, TableU, TableV })
        {
            const uint16_t a = rc::ld16(table + at), b = rc::ld16(table + at + 2);
            rc::st16(table + at, b);
            rc::st16(table + at + 2, a);
        }
    }
    rc::st32(TableOrder, 0);
}

// Rows as 0x6ffd4a writes them, two pixels per step.
void rows(const Arguments& a, x86::reg32 start, int pairs)
{
    const uint8_t* const g = win32::g_guestBase;
    const int16_t* const ty = reinterpret_cast<const int16_t*>(g + TableY);
    const int16_t* const tu = reinterpret_cast<const int16_t*>(g + TableU);
    const int16_t* const tv = reinterpret_cast<const int16_t*>(g + TableV);
    const x86::reg32 chromaPitch = x86::reg32(x86::sreg32(a.yPitch) >> 1), vOffset = a.v - a.u;
    x86::reg32 y = a.y, u = a.u;
    for (x86::reg32 row = 0; row < a.height; ++row)
    {
        const uint8_t* const ys = g + y;
        const uint8_t* const us = g + u;
        const uint8_t* const vs = g + x86::reg32(u + vOffset);
        uint8_t* const out = win32::g_guestBase + x86::reg32(start + row * a.outPitch);
        for (int i = 0; i < pairs; ++i)
        {
#if defined(__ARM_NEON)
            const int16x4_t c = vadd_s16(vld1_s16(tu + 4 * us[i]), vld1_s16(tv + 4 * vs[i]));
            const int16x4_t p0 = vadd_s16(vld1_s16(ty + 4 * ys[2 * i]), c);
            const int16x4_t p1 = vadd_s16(vld1_s16(ty + 4 * ys[2 * i + 1]), c);
            vst1_u8(out + 8 * i, vqmovun_s16(vcombine_s16(p0, p1)));
#else
            for (int k = 0; k < 2; ++k)
                for (int w = 0; w < 4; ++w)
                {
                    const int sum = int16_t(ty[4 * ys[2 * i + k] + w] + tu[4 * us[i] + w] + tv[4 * vs[i] + w]);
                    out[8 * i + 4 * k + w] = uint8_t(sum < 0 ? 0 : sum > 255 ? 255 : sum);
                }
#endif
        }
        y += a.yPitch;
        if (row & 1)
            u += chromaPitch;
    }
}

void body(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 entry = cpu.esp;
    if (!NFSU_NATIVE("movies_convert_native") || rc::ld32(entry + 36) != 1 || (rc::ld32(entry + 16) & 1))
    {
        sub_700150_guest(app, cpu);
        return;
    }
    const Arguments a = read(entry);
    cpu.esp = entry + 4;
    if (a.outPitch & 7)
    {
        cpu.eax = 3;
        return;
    }
    if ((a.out & 7) && (a.out & 3))
    {
        cpu.eax = 4;
        return;
    }
    cpu.eax = 0;
    x86::reg32 start, pixels;
    span(a, start, pixels);
    if (x86::sreg32(pixels) < 2 || x86::sreg32(a.height) < 1)
        return;
    if (rc::ld32(TableOrder) != 0)
        swapTables();
    rows(a, start, int(pixels / 2));
    rc::st32(entry + 36, a.height);
    cpu.mmx.init();
}

void declare(x86::CPU& cpu, shadow::Spans& spans)
{
    const x86::reg32 entry = cpu.esp;
    const Arguments a = read(entry);
    spans.push_back(shadow::Span{entry + 36, 4, false});
    spans.push_back(shadow::Span{TableOrder, 4, false});
    for (x86::reg32 table : { TableY, TableU, TableV })
        spans.push_back(shadow::Span{table, 0x800, false});
    x86::reg32 start, pixels;
    span(a, start, pixels);
    if (x86::sreg32(pixels) >= 2 && x86::sreg32(a.height) >= 1 && a.height <= 4096)
        for (x86::reg32 row = 0; row < a.height; ++row)
            spans.push_back(shadow::Span{x86::reg32(start + row * a.outPitch), pixels * 4, false});
}

const shadow::Spec spec = {"sub_700150", shadow::Ebx | shadow::Ebp | shadow::Esi | shadow::Edi, 0, false, false};

}

void sub_700150(win32::WinApplication* app, x86::CPU& cpu)
{
    shadow::call(spec, app, cpu, body, sub_700150_guest, declare);
}

}

}
