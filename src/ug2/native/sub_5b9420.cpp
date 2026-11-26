// Profile control save (sub_5b9420, cdecl, one argument): the pad's record in the profile is always the Custom layout.

#include "../controlslink.h"
#include <native/common.h>

namespace ug2
{

void sub_5b9420_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_5b9420(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 out = rc::ld32(cpu.esp + 4);
    sub_5b9420_guest(app, cpu);
    if (cpu.terminate)
        return;
    controlslink::patchProfileSave(out);
}

}

}
