// The game's start: a registry Language value whose files are missing is set aside for the default.

#include "language.h"
#include "translations.h"
#include <lib/registry.h>
#include <platform/log.h>
#include <cstring>

namespace ug2 { namespace language {

void configure(const std::string& gameFolder)
{
    win32::registry::Key* game = win32::registry::find(win32::registry::root(win32::HKEY_LOCAL_MACHINE),
                                                       "Software\\EA Games\\Need for Speed Underground 2", true);
    char text[64] = {};
    x86::reg32 type = 0, size = sizeof(text) - 1;
    if (win32::registry::query(game, "Language", &type, text, &size) != 0 || type != win32::registry::REG_SZ)
    {
        pf::logInfo("language: no registry value, the game's default");
        translations::setLanguage(0);
        return;
    }
    const int index = indexOfRegistryName(text);
    if (index <= 0 || available(gameFolder, index))
    {
        pf::logInfo("language: %s", text);
        translations::setLanguage(index < 0 ? 0 : index);
        return;
    }
    translations::setLanguage(0);
    pf::logError("language: %s, but LANGUAGES/%s.BIN or memcard/LOCALE_%s.loc is not in the game folder: %s instead", text,
                 Names[index], Names[index], DefaultRegistryName);
    win32::registry::setOverride(game, "Language", win32::registry::REG_SZ, DefaultRegistryName,
                                 x86::reg32(strlen(DefaultRegistryName) + 1));
}

}}
