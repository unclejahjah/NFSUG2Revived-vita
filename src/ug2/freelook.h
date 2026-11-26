#ifndef UG2_FREELOOK_H_
#define UG2_FREELOOK_H_

#include <lib/winapp.h>

// Free look for the race chase camera: the unbound right stick's direction sets the view yaw; down looks behind.

namespace ug2 { namespace freelook {

// The chase camera update running, and its look-at call's esp.
struct Update
{
    x86::reg32 camera;
    x86::reg32 lookAtEsp;
};

// Chase camera update starts at this esp; returns the outer update.
Update enter(x86::reg32 camera, x86::reg32 esp);
void leave(const Update& outer);

// Turns the chase camera before its look-at matrix.
void beforeLookAt(win32::WinApplication* app, x86::CPU& cpu);

}}

#endif /* !UG2_FREELOOK_H_ */
