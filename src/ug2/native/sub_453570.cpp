// Chase camera update (sub_453570, thiscall): free look reads the stick and marks its look-at call.

#include "../freelook.h"
#include <native/common.h>

namespace ug2
{

void sub_453570_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_453570(win32::WinApplication* app, x86::CPU& cpu)
{
    const freelook::Update outer = freelook::enter(cpu.ecx, cpu.esp);
    sub_453570_guest(app, cpu);
    freelook::leave(outer);
}

}

}
