// Native sub_4013f0: float 4x4 matrix multiply (cdecl: out, A, B). On by default; rounding changes the race outcome (matrix4_native=off).

#include <native/common.h>
#include <native/matrix4.h>
#include <native/shadow.h>
#include <platform/switches.h>
#include <platform/cached.h>
#include <platform/config.h>

namespace ug2
{

void sub_4013f0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

void body(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!NFSU_NATIVE("matrix4_native"))
    {
        sub_4013f0_guest(app, cpu);
        return;
    }
    const x86::reg32 esp = cpu.esp;
    const x86::reg32 destination = rc::ld32(esp + 4);
    matrix4::multiply(rc::host(rc::ld32(esp + 8)), rc::host(rc::ld32(esp + 12)), rc::host(destination));
    cpu.eax = destination;
    cpu.esp = esp + 4;
}

void declare(x86::CPU& cpu, shadow::Spans& spans)
{
    shadow::Span span = {argument(cpu, 0), 64, false};
    span.matmulN = 4;
    span.matmulA = argument(cpu, 1);
    span.matmulB = argument(cpu, 2);
    spans.push_back(span);
}

// The only caller, sub_43c990, returns at once and has no register returns in ug2_layout.h: only eax (output pointer) is kept.
const shadow::Spec spec = {"sub_4013f0", shadow::Eax, 0, false, false};

}

void sub_4013f0(win32::WinApplication* app, x86::CPU& cpu)
{
    shadow::call(spec, app, cpu, body, sub_4013f0_guest, declare);
}

}}
