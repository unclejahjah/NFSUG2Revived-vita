// Chase camera update (sub_453570, thiscall): the fork's stick camera (camera_stick) or else free look reads the stick
// and marks its look-at call.

#include "../freelook.h"
#include "../camera_stick.h"
#include <native/common.h>

namespace ug2
{

void sub_453570_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_453570(win32::WinApplication* app, x86::CPU& cpu)
{
    if (cameraStickOn(cpu))
    {
        sub_453570_guest(app, cpu);
        cameraStickEnd();
        return;
    }
    const freelook::Update outer = freelook::enter(cpu.ecx, cpu.esp);
    sub_453570_guest(app, cpu);
    freelook::leave(outer);
}

}

}
