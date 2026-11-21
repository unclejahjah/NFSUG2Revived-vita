// Native sub_5bd140: shader-constant upload of the draw queue. The fixed-function light path stays guest code by design (partial conversion).

#include <native/common.h>
#include <native/flush_context.h>
#include <native/shadow.h>
#include <platform/switches.h>
#include <platform/cached.h>
#include <platform/config.h>
#include <winapi/d3d9_native.h>

namespace ug2
{

void sub_5bd140_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

void body(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!NFSU_NATIVE("light_upload_native"))
    {
        sub_5bd140_guest(app, cpu);
        return;
    }
    const x86::reg32 group = flush::ld32<0x870910>(cpu);
    const x86::reg32 kind = rc::ld32(group + 4);
    if (kind == 5 && rc::ld8(group + 0x18) == 0)
    {
        sub_5bd140_guest(app, cpu);
        return;
    }
    if (kind != 5 && kind != 4)
    {
        cpu.eax = kind;
        cpu.esp += 4;
        return;
    }
    const x86::reg32 context = argument(cpu, 0);
    const x86::reg32 table = rc::ld32(group + 0x1c);
    const x86::reg32 effect = rc::ld32(group + 0x20);
    // No CallGroup (UG2 opens no deferral window); bridge calls never write guest memory.
    win32::d3d9::bridge::effectSetVectorArray(app, cpu, effect, rc::ld32(table + 0x4fc), flush::lightArray(cpu, context), 10);
    cpu.eax = win32::d3d9::bridge::effectSetVector(app, cpu, effect, rc::ld32(table + 0x5ec), flush::lightVector(cpu, context));
    cpu.esp += 4;
}

void declare(x86::CPU&, shadow::Spans&) {}

// eax, ecx and edx are dead after the only call site (5cf53a).
const shadow::Spec spec = {"sub_5bd140", shadow::Ebx | shadow::Esp | shadow::Ebp | shadow::Esi | shadow::Edi, 0, false, true};

}

void sub_5bd140(win32::WinApplication* app, x86::CPU& cpu)
{
    shadow::call(spec, app, cpu, body, sub_5bd140_guest, declare);
}

}}
