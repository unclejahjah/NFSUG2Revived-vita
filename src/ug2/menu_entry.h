#ifndef UG2_MENU_ENTRY_H_
#define UG2_MENU_ENTRY_H_

#include <lib/winapp.h>

// Vita Settings in the game's own screens: an icon left of Career on the main menu, the groups of options_menu.cpp in
// the game's options carousel and their settings in its options list.

namespace ug2 { namespace menu {

// Before guest code runs, after options::configure and textentry::configure.
void configure(win32::WinApplication& app);

}}

#endif /* !UG2_MENU_ENTRY_H_ */
