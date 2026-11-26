// PC help bar Hide (sub_54e6b0, thiscall): the PS2 bar group follows.

#include "../psbar.h"
#include <native/common.h>

namespace ug2
{

void sub_54e6b0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_54e6b0(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 bar = cpu.ecx;
    sub_54e6b0_guest(app, cpu);
    if (cpu.terminate)
        return;
    const x86::CPU saved = cpu;
    psbar::visibility(app, cpu, bar, false);
    cpu = saved;
}

}

}
