#ifndef LAUNCHER_MODS_EDIT_H_
#define LAUNCHER_MODS_EDIT_H_

// The launcher's side of the mod registry: the Mods tab's rows and writing the mods' ini files.

#include "cfgui.h"
#include "mods_registry.h"
#include <string>
#include <vector>

namespace ug2 { namespace mods {

namespace ini
{
// Sets the value, keeping any comment, the line ends and the final break; writes through file.tmp.
bool set(const std::string& file, const char* section, const char* key, const std::string& number,
         const char* comment);
}

// Every key of every source; supported keys carry their feature's Id in ModView::id.
std::vector<cfgui::ModView> views(const std::string& gameFolder);
// Writes the Mods tab's choice to the mod's ini.
bool save(const Mod& mod, const std::string& gameFolder, bool on);

}}

#endif
