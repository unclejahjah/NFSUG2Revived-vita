#ifndef UG2_PSBAR_H_
#define UG2_PSBAR_H_

#include <lib/winapp.h>

// The PS2 bottom bar: the PS2 bar group copied into the PC help bar's package, filled with the profile name, Help and one prompt.

namespace ug2 { namespace psbar {

// Queues the package rebuild; call once at start.
void install();
// Around the PC bar fill (thiscall on the bar; flags, screen name).
void beforeFill(win32::WinApplication* app, x86::CPU& cpu);
void afterFill(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 bar);
// After the PC bar's Show and Hide (thiscall on the bar).
void visibility(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 bar, bool show);
// Per frame: follows Show and Hide the game inlines.
void frame(win32::WinApplication* app, x86::CPU& cpu);

}}

#endif /* !UG2_PSBAR_H_ */
