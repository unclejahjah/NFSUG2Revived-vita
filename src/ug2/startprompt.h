#ifndef UG2_STARTPROMPT_H_
#define UG2_STARTPROMPT_H_

#include <lib/winapp.h>

// The start splash asks for START, not the PC's Enter key, using the game's own translated text.

namespace ug2 { namespace startprompt {

// Per frame until the language table is patched.
void frame();

}}

#endif /* !UG2_STARTPROMPT_H_ */
