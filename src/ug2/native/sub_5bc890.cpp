// Native sub_5bc890: group begin, name, 0x86bd18 and 0x870894 textures, projective matrix or float (thiscall).

#include <native/common.h>
#include <native/flush_context.h>
#include <native/group_begin.h>
#include <platform/switches.h>

namespace ug2
{

void sub_5bc890_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

void body(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!NFSU_NATIVE("sub_5bc890_native")) [[unlikely]]
    {
        sub_5bc890_guest(app, cpu);
        return;
    }
    const bool fixed = rc::ld8(cpu.ecx + 0x18) == 0;
    group::setTexture(app, cpu, 0x59c, group::nameTexture());
    group::setTexture(app, cpu, 0x54c, flush::ld32<0x86bd18>(cpu));
    group::setTexture(app, cpu, 0x72c, flush::ld32<0x870894>(cpu));
    if (fixed)
    {
        group::projectiveMatrix(app, cpu);
        group::fogColour(app, cpu);
    }
    else
    {
        group::setTexture(app, cpu, 0x59c, group::nameTexture());
        group::setTexture(app, cpu, 0x54c, flush::ld32<0x86bd18>(cpu));
        group::setTexture(app, cpu, 0x72c, flush::ld32<0x870894>(cpu));
        const x86::reg32 current = flush::ld32<group::Current>(cpu);
        group::br::effectSetFloat(app, cpu, rc::ld32(current + 0x20), rc::ld32(rc::ld32(current + 0x1c) + 0x754),
                                  rc::ldf32(0x7a27e4));
    }
    cpu.esp += 4;
}

}

void sub_5bc890(win32::WinApplication* app, x86::CPU& cpu)
{
    body(app, cpu);
}

}}
