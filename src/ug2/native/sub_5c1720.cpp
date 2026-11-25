// Pad capture, a hat off centre (sub_5c1720, thiscall (hat), ret 4): Customize's bind wait takes it instead.

#include "../controlsshell.h"
#include <native/common.h>

namespace ug2
{

void sub_5c1720_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_5c1720(win32::WinApplication* app, x86::CPU& cpu)
{
    if (controlsshell::captureHat(cpu.ecx, int(rc::ld32(cpu.esp + 4))))
    {
        cpu.esp += 8;
        return;
    }
    sub_5c1720_guest(app, cpu);
}

}

}
