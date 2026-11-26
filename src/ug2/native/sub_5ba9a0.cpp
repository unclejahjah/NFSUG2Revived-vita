// Look-at matrix (sub_5ba9a0, cdecl: out, eye, target, up): the stick camera or free look turns the chase
// camera first.

#include "../freelook.h"
#include "../camera_stick.h"
#include <native/common.h>

namespace ug2
{

void sub_5ba9a0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_5ba9a0(win32::WinApplication* app, x86::CPU& cpu)
{
    cameraStickLookAt(cpu);
    freelook::beforeLookAt(app, cpu);
    sub_5ba9a0_guest(app, cpu);
}

}

}
