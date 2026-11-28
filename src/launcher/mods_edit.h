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
struct Line
{
    const char* section;
    const char* key;
    std::string value;          // empty: remove the key
    const char* comment;        // written after a new line's value; null for none
};
// Sets the values, keeping any comment, the line ends and the final break; an empty value removes the key's line.
// Writes through file.tmp.
bool set(const std::string& file, const std::vector<Line>& lines);
bool set(const std::string& file, const char* section, const char* key, const std::string& number,
         const char* comment);
}

// The value that leaves the game as it is (modreq::Setting::vanilla); empty for a key's Vanilla choice.
std::string vanillaValue(const Mod& mod);
// A new settings file with every supported key at its vanilla value (an empty one left out), an unsupported one at
// the mod's own value, and key (when given) at value.
bool writeFresh(const Source& src, const std::string& file, const char* section, const char* key,
                const std::string& value);

// Every key of every source; supported keys carry their feature's Id in ModView::id.
std::vector<cfgui::ModView> views(const std::string& gameFolder);
// Writes the Mods tab's value to the mod's ini; a missing one is created whole (writeFresh).
bool save(const Mod& mod, const std::string& gameFolder, const std::string& value);
bool save(const Mod& mod, const std::string& gameFolder, bool on);

}}

#endif
