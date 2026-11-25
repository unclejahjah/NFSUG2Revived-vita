// Token skip (sub_50bac0, cdecl; text, flags): draws or measures a button icon token, then runs the game.

#include "../psprompt.h"
#include <native/common.h>

namespace ug2
{

void sub_50bac0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_50bac0(win32::WinApplication* app, x86::CPU& cpu)
{
    psprompt::beforeTokenSkip(app, cpu);
    sub_50bac0_guest(app, cpu);
}

}

}
