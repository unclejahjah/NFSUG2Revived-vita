#ifndef UG2_CONTROLSSHELL_H_
#define UG2_CONTROLSSHELL_H_

#include <lib/winapp.h>

// Options > Controls on own screens (Controls, Joypad Configuration, Customize), with Customize's bind wait.

namespace ug2 { namespace controlsshell {

// Before guest code runs.
void configure(win32::WinApplication& app);
// Per frame on the game thread.
void frame(win32::WinApplication* app, x86::CPU& cpu);
// The PC capture found a button, axis or hat; false: not ours.
bool captureButton(x86::reg32 slot, int button);
bool captureAxis(x86::reg32 slot, int32_t axisId, int direction);
bool captureHat(x86::reg32 slot, int hat);
// A bind wait holds the pad, the garage stick included.
bool inputHeld();

}}

#endif /* !UG2_CONTROLSSHELL_H_ */
