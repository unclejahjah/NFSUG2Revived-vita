// PC help bar fill (sub_54e6e0, thiscall; flags, screen name): the bar is filled as the PS2 bottom bar.

#include "../carfilter.h"
#include "../psbar.h"
#include <native/common.h>

namespace ug2
{

void sub_54e6e0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_54e6e0(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 bar = cpu.ecx, screen = argument(cpu, 1);
    psbar::beforeFill(app, cpu);
    sub_54e6e0_guest(app, cpu);
    if (cpu.terminate)
        return;
    const x86::CPU saved = cpu;
    psbar::afterFill(app, cpu, bar);
    carfilter::afterFill(app, cpu, screen);
    cpu = saved;
}

}

}
