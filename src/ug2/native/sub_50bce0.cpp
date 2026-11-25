// Line width (sub_50bce0, thiscall on the font): marks its token skip as a width measure.

#include "../psprompt.h"
#include <native/common.h>

namespace ug2
{

void sub_50bce0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_50bce0(win32::WinApplication* app, x86::CPU& cpu)
{
    const psprompt::Scope saved = psprompt::enterText(psprompt::Width, cpu.ecx);
    sub_50bce0_guest(app, cpu);
    psprompt::leaveText(saved);
}

}

}
