// Native sub_6e3b10: builds each bone's scaled rotation-translation matrix from a keyframe and multiplies it by its parent's (thiscall, ret 12). sub_6e2d60 and the matrix call inlined.

#include <native/common.h>
#include <native/fast.h>
#include <native/shadow.h>
#include <winapi/d3d9_native.h>

namespace ug2
{

void sub_6e3b10_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

using x86::reg32;

constexpr reg32 MultiplySlot = 0x811e34, MultiplyWrapper = 0x6efa00, One = 0x784250;

// sub_6e2d60: scale, quaternion, translation to matrix.
void composeMatrix(reg32 m, reg32 e, Real one)
{
    const Real sx = ld(e), sy = ld(e + 4), sz = ld(e + 8);
    const Real qx = ld(e + 0x10), qy = ld(e + 0x14), qz = ld(e + 0x18), qw = ld(e + 0x1c);
    const reg32 tx = rc::ld32(e + 0x20), ty = rc::ld32(e + 0x24), tz = rc::ld32(e + 0x28);

    Real x2 = qx;
    x2 += qx;
    Real y2 = qy;
    y2 += qy;
    Real z2 = qz;
    z2 += qz;
    const float zf = float(z2);
    Real t = x2;
    t *= qw;
    const float a = float(t);
    t = qw;
    t *= y2;
    const float b = float(t);
    t = Real(zf);
    t *= qw;
    const float c = float(t);
    t = x2;
    t *= qx;
    const float d = float(t);
    t = qx;
    t *= y2;
    const float ee = float(t);
    t = Real(zf);
    t *= qx;
    const float f = float(t);
    Real g = y2;
    g *= qy;
    Real h = Real(zf);
    h *= qy;
    Real i = Real(zf);
    i *= qz;

    Real r = i;
    r += g;
    r = one - r;
    r *= sx;
    rc::stf32(m, float(r));
    r = Real(ee);
    r -= Real(c);
    r *= sy;
    rc::stf32(m + 0x10, float(r));
    r = Real(f);
    r += Real(b);
    r *= sz;
    rc::stf32(m + 0x20, float(r));
    r = Real(ee);
    r += Real(c);
    r *= sx;
    rc::stf32(m + 4, float(r));
    r = i;
    r += Real(d);
    r = one - r;
    r *= sy;
    rc::stf32(m + 0x14, float(r));
    r = h;
    r = r - Real(a);
    r *= sz;
    rc::stf32(m + 0x24, float(r));
    r = Real(f);
    r -= Real(b);
    r *= sx;
    rc::stf32(m + 8, float(r));
    r = h;
    r += Real(a);
    r *= sy;
    rc::stf32(m + 0x18, float(r));
    r = g;
    r += Real(d);
    r = one - r;
    r *= sz;
    rc::stf32(m + 0x28, float(r));
    rc::st32(m + 0xc, 0);
    rc::st32(m + 0x1c, 0);
    rc::st32(m + 0x2c, 0);
    rc::st32(m + 0x30, tx);
    rc::st32(m + 0x34, ty);
    rc::st32(m + 0x38, tz);
    rc::st32(m + 0x3c, 0x3f800000);
}

signed char s_on;

void body(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!fast::enabled(s_on, "pose_native") || rc::ld32(MultiplySlot) != MultiplyWrapper) [[unlikely]]
    {
        sub_6e3b10_guest(app, cpu);
        return;
    }
    const reg32 self = cpu.ecx;
    const reg32 entries = argument(cpu, 0), matrices = argument(cpu, 1), mask = argument(cpu, 2);
    const int32_t count = int32_t(rc::ld32(self + 8));
    const Real one = ld(One);
    for (int32_t i = 0; i < count; ++i)
    {
        if (mask != 0 && !(rc::ld32(mask + reg32(i >> 5) * 4) & (1u << (i & 31))))
            continue;
        const reg32 e = entries + reg32(i) * 0x30, m = matrices + reg32(i) * 0x40;
        composeMatrix(m, e, one);
        const Real scale = ld(e + 0xc);
        for (reg32 offset = 0; offset < 0x30; offset += 0x10)
        {
            Real v = ld(m + offset);
            v *= scale;
            rc::stf32(m + offset, float(v));
        }
        const int32_t parent = int32_t(rc::ld32(self + 0x1c + reg32(i) * 0x70));
        if (parent >= 0)
        {
            float* const out = reinterpret_cast<float*>(rc::host(m));
            win32::d3d9::bridge::matrixMultiply(out, reinterpret_cast<const float*>(rc::host(matrices + reg32(parent) * 0x40)), out);
        }
    }
    cpu.esp += 16;
}

void declare(x86::CPU& cpu, shadow::Spans& spans)
{
    const int32_t count = int32_t(rc::ld32(cpu.ecx + 8));
    if (count > 0)
        spans.push_back({argument(cpu, 1), reg32(count) * 0x40, false});
}

const shadow::Spec spec = {"sub_6e3b10", shadow::Esi, 0, false, false};

}

void sub_6e3b10(win32::WinApplication* app, x86::CPU& cpu)
{
    shadow::call(spec, app, cpu, body, sub_6e3b10_guest, declare);
}

}}
