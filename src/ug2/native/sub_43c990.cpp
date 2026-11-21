// Native sub_43c990: cdecl wrapper of sub_4013f0 with the matrices swapped (out, B, A): float 4x4 multiply, kernel inlined.

#include <native/common.h>
#include <native/fast.h>
#include <native/matrix4.h>
#include <native/shadow.h>

namespace ug2
{

void sub_43c990_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

signed char s_on;

void body(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!fast::enabled(s_on, "matrix4_native")) [[unlikely]]
    {
        sub_43c990_guest(app, cpu);
        return;
    }
    const x86::reg32 out = argument(cpu, 0);
    matrix4::multiply(rc::host(argument(cpu, 2)), rc::host(argument(cpu, 1)), rc::host(out));
    cpu.eax = out;
    cpu.esp += 4;
}

void declare(x86::CPU& cpu, shadow::Spans& spans)
{
    shadow::Span span = {argument(cpu, 0), 64, false};
    span.matmulN = 4;
    span.matmulA = argument(cpu, 2);
    span.matmulB = argument(cpu, 1);
    spans.push_back(span);
}

const shadow::Spec spec = {"sub_43c990", shadow::Eax | shadow::Ebx | shadow::Ebp | shadow::Esi | shadow::Edi, 0, false, false};

}

void sub_43c990(win32::WinApplication* app, x86::CPU& cpu)
{
    shadow::call(spec, app, cpu, body, sub_43c990_guest, declare);
}

}}
