// The default start-up hook: no mod features, nothing to read. A build with them defines its own.

#include "mods_hook.h"

namespace ug2 {

__attribute__((weak)) std::vector<std::string> readModSettings(const std::string&)
{
    return std::vector<std::string>();
}

std::string modUnreadable(const std::string& mod, const std::string& iniPath)
{
    return "Mod " + mod + " was not able to be loaded as " + iniPath + " was unable to be read.";
}

}
