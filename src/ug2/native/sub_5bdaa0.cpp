// Native sub_5bdaa0: headlight projection matrix and colour of one mesh (cdecl: world, light).
// The fixed-function path runs the guest body.

#include <native/common.h>
#include <native/flush_context.h>
#include <native/shadow.h>
#include <platform/switches.h>
#include <platform/cached.h>
#include <platform/config.h>
#include <winapi/d3d9_native.h>

namespace ug2
{

void sub_5bdaa0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

const float* matrixAt(x86::reg32 address)
{
    return reinterpret_cast<const float*>(rc::host(address));
}

void body(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 group = flush::ld32<0x870910>(cpu);
    if (rc::ld8(group + 0x18) == 0 || !NFSU_NATIVE("headlight_pass_native"))
    {
        sub_5bdaa0_guest(app, cpu);
        return;
    }
    const x86::reg32 world = argument(cpu, 0);
    const float* const projection = flush::viewMatricesAt(cpu, argument(cpu, 1)) + 32;
    float offset[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    offset[12] = flush::ldf32<0x80052c>(cpu);
    offset[13] = flush::ldf32<0x800530>(cpu);
    float product[16];
    win32::d3d9::bridge::matrixMultiply(matrixAt(world), projection, product);
    win32::d3d9::bridge::matrixMultiply(product, offset, product);
    const x86::reg32 effect = rc::ld32(group + 0x20);
    const x86::reg32 table = rc::ld32(group + 0x1c);
    win32::d3d9::bridge::effectSetMatrix(app, cpu, effect, rc::ld32(table + 0x5c4), product);

    x86::reg32 colour = 0;
    const x86::reg32 owner = flush::ld32<0x8900b4>(cpu);
    if (owner != 0)
    {
        const x86::reg32 car = rc::ld32(owner + 4);
        if (car != 0)
        {
            const x86::reg32 lights = rc::ld32(car + 0x554);
            if (lights != 0)
                colour = rc::ld32(lights + 0x380);
        }
    }
    const Real scale = ld(0x787af0);
    float vector[4];
    vector[0] = float(Real(x86::sreg32(colour & 0xff)) * scale);
    vector[1] = float(Real(x86::sreg32((colour >> 8) & 0xff)) * scale);
    vector[2] = float(Real(x86::sreg32((colour >> 16) & 0xff)) * scale);
    vector[3] = 0;
    cpu.eax = win32::d3d9::bridge::effectSetVector(app, cpu, effect, rc::ld32(table + 0x574), vector);
    cpu.esp += 4;
}

void declare(x86::CPU&, shadow::Spans&) {}

// eax, ecx, edx dead after the only call site, 0x5cf526.
const shadow::Spec spec = {"sub_5bdaa0", shadow::Ebx | shadow::Esp | shadow::Ebp | shadow::Esi | shadow::Edi, 0, false, true};

}

void sub_5bdaa0(win32::WinApplication* app, x86::CPU& cpu)
{
    shadow::call(spec, app, cpu, body, sub_5bdaa0_guest, declare);
}

}}
