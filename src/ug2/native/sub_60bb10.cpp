// Native sub_60bb10: updates a scene node's world matrix and position from its parent, recursing up through dirty parents (thiscall, ret 0). Callees inlined.

#include <native/common.h>
#include <native/fast.h>
#include <native/matrix4.h>
#include <native/shadow.h>
#include <platform/switches.h>

namespace ug2
{

void sub_60bb10_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

using x86::reg32;

// Copy of sub_4012b0, middle lanes via float.
void copyMatrix(reg32 out, reg32 in)
{
    for (reg32 row = 0; row < 64; row += 16)
    {
        rc::st32(out + row, rc::ld32(in + row));
        rc::stf32(out + row + 4, float(ld(in + row + 4)));
        rc::stf32(out + row + 8, float(ld(in + row + 8)));
        rc::st32(out + row + 12, rc::ld32(in + row + 12));
    }
}

void update(reg32 self)
{
    reg32 parent = rc::ld32(self + 0xe0);
    if (parent == 0)
    {
        copyMatrix(self + 0x10, self + 0x50);
        const reg32 x = rc::ld32(self + 0xa0);
        const float y = float(ld(self + 0xa4)), z = float(ld(self + 0xa8));
        rc::st32(self + 0x90, x);
        rc::stf32(self + 0x94, y);
        rc::stf32(self + 0x98, z);
        rc::st32(self + 0xe4, 0);
        return;
    }
    if (rc::ld32(parent + 0xe4))
        update(parent);
    matrix4::multiply(rc::host(self + 0x50), rc::host(parent + 0x10), rc::host(self + 0x10));
    parent = rc::ld32(self + 0xe0);
    if (rc::ld32(parent + 0xe4))
        update(parent);
    float bx, by, bz;
    fast::transform<false>(parent + 0x10, self + 0xa0, bx, by, bz);
    parent = rc::ld32(self + 0xe0);
    if (rc::ld32(parent + 0xe4))
        update(parent);
    const Real p44 = ld(parent + 0x44), p48 = ld(parent + 0x48);
    Real tx = Real(bx);
    tx -= ld(parent + 0x40);
    bx = float(tx);
    Real ty = Real(by);
    ty -= p44;
    by = float(ty);
    Real tz = Real(bz);
    tz -= p48;
    bz = float(tz);
    parent = rc::ld32(self + 0xe0);
    if (rc::ld32(parent + 0xe4))
        update(parent);
    Real x = Real(bx);
    x += ld(parent + 0x90);
    Real y = ld(parent + 0x94);
    y += Real(by);
    Real z = ld(parent + 0x98);
    z += Real(bz);
    rc::stf32(self + 0x90, float(x));
    rc::stf32(self + 0x98, float(z));
    rc::stf32(self + 0x94, float(y));
    rc::st32(self + 0xe4, 0);
}

signed char s_on;

void body(win32::WinApplication* app, x86::CPU& cpu)
{
    static signed char s_matrix, s_vector;
    if (!fast::enabled(s_on, "hierarchy_native") || !fast::enabled(s_matrix, "matrix4_native") ||
        !fast::enabled(s_vector, "vec_helpers_native")) [[unlikely]]
    {
        sub_60bb10_guest(app, cpu);
        return;
    }
    update(cpu.ecx);
    cpu.esp += 4;
}

void declare(x86::CPU& cpu, shadow::Spans& spans)
{
    reg32 node = cpu.ecx;
    for (int depth = 0; node != 0 && depth < 16; ++depth)
    {
        spans.push_back({node + 0x10, 0x40, false});
        spans.push_back({node + 0x90, 12, false});
        spans.push_back({node + 0xe4, 4, false});
        node = rc::ld32(node + 0xe0);
    }
}

const shadow::Spec spec = {"sub_60bb10", shadow::Ebx | shadow::Esi | shadow::Edi | shadow::Ebp, 0, false, false};

}

void sub_60bb10(win32::WinApplication* app, x86::CPU& cpu)
{
    shadow::call(spec, app, cpu, body, sub_60bb10_guest, declare);
}

}}
