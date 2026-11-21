// Free roam with other players (NFSU2 Revived's free-roam mod), run where the PC's NFSU2FreeRoam.asi runs it: the
// call at 0x581475 in GameLoop, the second of two calls to the empty debug function 0x4022C0 between
// World::DoTimestep and the camera, audio and render (the first, 0x581470, is where ExtraOptions hooks on PC).
// 0x4022C0 is the compiler's shared empty function, called from many places.
#include <freeroam.h>
#include <lib/winapp.h>
#include <recomp.h>

namespace ug2
{

void sub_4022c0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_4022c0(win32::WinApplication* app, x86::CPU& cpu)
{
    // That call writes its return address (RETURN_ADDRESS_SITES in tools/recompiler/games/ug2.py); the others leave
    // the slot as it was, so it is cleared here: the call at 0x581470 finds this one's there a frame later.
    const x86::reg32 HookReturn = 0x0058147A;   // after the call at 0x581475
    if (rc::ld32(cpu.esp) == HookReturn)
    {
        rc::st32(cpu.esp, 0);
        freeroam::frame(app, cpu);   // src/lib/winapi/revived/roam/roam_vita.cpp
    }
    sub_4022c0_guest(app, cpu);
}

}

}
