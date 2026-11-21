// Native sub_5bc7b0: group begin, projective texture matrix (thiscall).

#include <native/common.h>
#include <native/flush_context.h>
#include <native/group_begin.h>
#include <platform/switches.h>

namespace ug2
{

void sub_5bc7b0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

void body(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!NFSU_NATIVE("sub_5bc7b0_native")) [[unlikely]]
    {
        sub_5bc7b0_guest(app, cpu);
        return;
    }
    const bool fixed = rc::ld8(cpu.ecx + 0x18) == 0;
    group::setTexture(app, cpu, 0x72c, flush::ld32<0x870894>(cpu));
    if (fixed)
        group::projectiveMatrix(app, cpu);
    cpu.esp += 4;
}

}

void sub_5bc7b0(win32::WinApplication* app, x86::CPU& cpu)
{
    body(app, cpu);
}

}}
