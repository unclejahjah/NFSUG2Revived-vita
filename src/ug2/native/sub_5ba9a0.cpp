// Look-at matrix (sub_5ba9a0, cdecl: out, eye, target, up): free look turns the chase camera first.

#include "../freelook.h"
#include <native/common.h>

namespace ug2
{

void sub_5ba9a0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_5ba9a0(win32::WinApplication* app, x86::CPU& cpu)
{
    freelook::beforeLookAt(app, cpu);
    sub_5ba9a0_guest(app, cpu);
}

}

}
