// Native sub_5d6240: scenery instance (this) rotation rows, nine s16 at +0x2c..+0x3c scaled by a constant, to a 3x4 float matrix (thiscall, ret 4).

#include <native/common.h>
#include <native/shadow.h>

namespace ug2
{

void sub_5d6240_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

const x86::reg32 Scale = 0x7a51dc;

inline void put(x86::reg32 out, x86::reg32 inst, Real scale, x86::reg32 first, x86::reg32 second, x86::reg32 third)
{
    rc::stf32(out, float(Real(x86::sreg32(int16_t(rc::ld16(inst + first)))) * scale));
    rc::stf32(out + 4, float(Real(x86::sreg32(int16_t(rc::ld16(inst + second)))) * scale));
    rc::stf32(out + 8, float(Real(x86::sreg32(int16_t(rc::ld16(inst + third)))) * scale));
    rc::st32(out + 12, 0);
}

void body(win32::WinApplication*, x86::CPU& cpu)
{
    const x86::reg32 inst = cpu.ecx;
    const x86::reg32 out = argument(cpu, 0);
    const Real scale = ld(Scale);
    put(out, inst, scale, 0x2c, 0x2e, 0x30);
    put(out + 0x10, inst, scale, 0x32, 0x34, 0x36);
    put(out + 0x20, inst, scale, 0x38, 0x3a, 0x3c);
    // The guest reuses its argument slot as scratch and leaves the last integer there.
    rc::st32(cpu.esp + 4, x86::reg32(int32_t(int16_t(rc::ld16(inst + 0x38)))));
    cpu.esp += 8;
}

void declare(x86::CPU& cpu, shadow::Spans& spans)
{
    spans.push_back({argument(cpu, 0), 0x30, false});
    spans.push_back({cpu.esp + 4, 4, false});
}

const shadow::Spec spec = {"sub_5d6240", 0, 0, false, false};

}

void sub_5d6240(win32::WinApplication* app, x86::CPU& cpu)
{
    shadow::call(spec, app, cpu, body, sub_5d6240_guest, declare);
}

}}
