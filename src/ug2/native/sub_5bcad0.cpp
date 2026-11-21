// Native sub_5bcad0: group begin, texture chosen by the 0x8654a4 mode (cdecl, no arguments).

#include <native/common.h>
#include <native/flush_context.h>
#include <native/group_begin.h>
#include <platform/switches.h>

namespace ug2
{

void sub_5bcad0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

void body(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!NFSU_NATIVE("sub_5bcad0_native")) [[unlikely]]
    {
        sub_5bcad0_guest(app, cpu);
        return;
    }
    group::setTexture(app, cpu, 0x2f4, rc::ld32(flush::ld32<0x8654a4>(cpu) == 6 ? 0x870858 : 0x87088c));
    cpu.esp += 4;
}

}

void sub_5bcad0(win32::WinApplication* app, x86::CPU& cpu)
{
    body(app, cpu);
}

}}
