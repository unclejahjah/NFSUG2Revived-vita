#ifndef UG2_MODS_HOOK_H_
#define UG2_MODS_HOOK_H_

// Start-up hook for the mod features built into the game: they read their own settings files here.

#include <string>
#include <vector>

namespace ug2 {

// Reads each mod feature's settings file; returns one line per file present but unreadable. None by default.
std::vector<std::string> readModSettings(const std::string& gameFolder);

std::string modUnreadable(const std::string& mod, const std::string& iniPath);

}

#endif
