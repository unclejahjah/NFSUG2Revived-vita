// Native sub_5bc6f0: group begin, name texture and fog colour binds (thiscall).

#include <native/common.h>
#include <native/flush_context.h>
#include <native/group_begin.h>
#include <platform/switches.h>

namespace ug2
{

void sub_5bc6f0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

void body(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!NFSU_NATIVE("sub_5bc6f0_native")) [[unlikely]]
    {
        sub_5bc6f0_guest(app, cpu);
        return;
    }
    const bool fixed = rc::ld8(cpu.ecx + 0x18) == 0;
    group::setTexture(app, cpu, 0x59c, group::nameTexture());
    group::setTexture(app, cpu, 0x54c, flush::ld32<0x86bd18>(cpu));
    if (fixed)
        group::fogColour(app, cpu);
    cpu.esp += 4;
}

}

void sub_5bc6f0(win32::WinApplication* app, x86::CPU& cpu)
{
    body(app, cpu);
}

}}
