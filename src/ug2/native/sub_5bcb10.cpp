// Native sub_5bcb10: group begin, texture transform flags of stages 0 to 3 cleared (cdecl, no arguments).

#include <native/common.h>
#include <native/group_begin.h>
#include <platform/switches.h>

namespace ug2
{

void sub_5bcb10_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

void body(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!NFSU_NATIVE("sub_5bcb10_native")) [[unlikely]]
    {
        sub_5bcb10_guest(app, cpu);
        return;
    }
    for (x86::reg32 stage = 0; stage < 4; ++stage)
        group::br::setTextureStageState(app, cpu, rc::ld32(group::Device), stage, 0x18, 0);
    cpu.esp += 4;
}

}

void sub_5bcb10(win32::WinApplication* app, x86::CPU& cpu)
{
    body(app, cpu);
}

}}
