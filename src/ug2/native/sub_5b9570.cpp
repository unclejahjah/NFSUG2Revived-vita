// Profile control load (sub_5b9570, cdecl, one argument): after the game fills its slots from the profile, the stored layout takes over.

#include "../controlslink.h"
#include <native/common.h>

namespace ug2
{

void sub_5b9570_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_5b9570(win32::WinApplication* app, x86::CPU& cpu)
{
    sub_5b9570_guest(app, cpu);
    if (cpu.terminate)
        return;
    controlslink::afterProfileLoad();
}

}

}
