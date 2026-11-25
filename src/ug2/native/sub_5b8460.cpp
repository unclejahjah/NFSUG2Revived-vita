// Pad capture, a button (sub_5b8460, thiscall, ret 4): Customize's bind wait takes it instead of the PC's write.

#include "../controlsshell.h"
#include <native/common.h>

namespace ug2
{

void sub_5b8460_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_5b8460(win32::WinApplication* app, x86::CPU& cpu)
{
    if (controlsshell::captureButton(cpu.ecx, int(rc::ld32(cpu.esp + 4))))
    {
        cpu.esp += 8;
        return;
    }
    sub_5b8460_guest(app, cpu);
}

}

}
