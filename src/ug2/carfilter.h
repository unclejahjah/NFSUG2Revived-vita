#ifndef UG2_CARFILTER_H_
#define UG2_CARFILTER_H_

#include <lib/winapp.h>

// The car select screens' filter row shows L1 and R1 icons in place of the PC mouse arrows.

namespace ug2 { namespace carfilter {

// Queues the package edits; call once at start.
void install();
// After a PC help bar fill (screen name), DEV builds only.
void afterFill(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 screenName);

}}

#endif /* !UG2_CARFILTER_H_ */
