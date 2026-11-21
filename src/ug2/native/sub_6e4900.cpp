// Native sub_6e4900: animation time (float) to a track index, rounding time -/+ 0.5 (cdecl: time; result in eax).

#include <native/anim.h>
#include <native/common.h>
#include <native/fast.h>
#include <native/shadow.h>

namespace ug2
{

void sub_6e4900_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

signed char s_on;

void body(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!fast::enabled(s_on, "anim_time_native")) [[unlikely]]
    {
        sub_6e4900_guest(app, cpu);
        return;
    }
    float adjusted;
    cpu.eax = anim::timeIndex(cpu, rc::ldf32(cpu.esp + 4), &adjusted);
    rc::stf32(cpu.esp + 4, adjusted); // the guest leaves the adjusted time in its argument slot
    cpu.esp += 4;
}

void declare(x86::CPU& cpu, shadow::Spans& spans)
{
    spans.push_back({cpu.esp + 4, 4, false});
}

const shadow::Spec spec = {"sub_6e4900", shadow::Eax, 0, false, false};

}

void sub_6e4900(win32::WinApplication* app, x86::CPU& cpu)
{
    shadow::call(spec, app, cpu, body, sub_6e4900_guest, declare);
}

}}
