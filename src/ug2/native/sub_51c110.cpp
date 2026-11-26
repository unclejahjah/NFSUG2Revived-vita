// FE string render (sub_51c110, thiscall on the font): marks its token skips as drawn text.

#include "../psprompt.h"
#include <native/common.h>

namespace ug2
{

void sub_51c110_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_51c110(win32::WinApplication* app, x86::CPU& cpu)
{
    const psprompt::Scope saved = psprompt::enterText(psprompt::Render, cpu.ecx);
    sub_51c110_guest(app, cpu);
    psprompt::leaveText(saved);
}

}

}
