#ifndef LAUNCHER_SAVE_H_
#define LAUNCHER_SAVE_H_

// What the manager's Save writes: the cfg and the mods' ini files.

#include "cfgui.h"
#include <string>

namespace ug2 { namespace launcher {

// Changed files are edited in place; an unreadable cfg or mod ini is written fresh from the screen's values.
void save(const cfgui::Configurator& ui, const std::string& configPath, pf::cfgfile::FileState configState,
          const std::string& gameFolder);

}}

#endif
