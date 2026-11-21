// Native sub_5d64d0: pixel size of a sphere (cull info, position, radius; cdecl) as k * r / distance, 0 when behind the view.

#include <native/common.h>
#include <native/shadow.h>
#include <cmath>

namespace ug2
{

void sub_5d64d0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

void body(win32::WinApplication*, x86::CPU& cpu)
{
    const x86::reg32 info = argument(cpu, 0);
    const x86::reg32 pos = argument(cpu, 1);
    const float radius = rc::ldf32(cpu.esp + 12);
    const Real r = Real(radius);
    cpu.esp += 4;

    const Real dx = ld(pos) - ld(info + 0x60);
    const Real dy = ld(pos + 4) - ld(info + 0x64);
    const Real dz = ld(pos + 8) - ld(info + 0x68);
    Real dot = dz * ld(info + 0x78);
    dot += dy * ld(info + 0x74);
    dot += dx * ld(info + 0x70);
    if (double(-r) > double(dot))
    {
        cpu.eax = 0;
        return;
    }
    cpu.ecx = info;
    Real sum = dx * dx;
    sum += dz * dz;
    sum += dy * dy;
    const float gap = float(Real(std::sqrt(sum)) - r);
    Real pixels = ld(info + 0x80);
    if (double(gap) > double(radius))
    {
        pixels /= Real(gap);
        pixels *= r;
    }
    const x86::reg64 wide = x86::reg64(x86::truncInteger64(double(pixels)));
    cpu.eax = x86::reg32(wide);
    cpu.edx = x86::reg32(wide >> 32);
}

void declare(x86::CPU&, shadow::Spans&)
{
}

const shadow::Spec spec = {"sub_5d64d0", shadow::Eax, 0, false, false};

}

void sub_5d64d0(win32::WinApplication* app, x86::CPU& cpu)
{
    shadow::call(spec, app, cpu, body, sub_5d64d0_guest, declare);
}

}}
