// Pad capture, an axis (sub_5b8250, thiscall (axis id, direction), ret 8): Customize's bind wait takes it instead.

#include "../controlsshell.h"
#include <native/common.h>

namespace ug2
{

void sub_5b8250_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_5b8250(win32::WinApplication* app, x86::CPU& cpu)
{
    if (controlsshell::captureAxis(cpu.ecx, int32_t(rc::ld32(cpu.esp + 4)), int(int32_t(rc::ld32(cpu.esp + 8)))))
    {
        cpu.esp += 12;
        return;
    }
    sub_5b8250_guest(app, cpu);
}

}

}
