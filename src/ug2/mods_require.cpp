// The mod settings and their requirements, shared by the game and the launcher.

#include "mods_require.h"
#include "exopts.h"
#include <cstring>
#include <sys/stat.h>
#include <vector>

namespace ug2 { namespace modreq {

namespace
{

bool exists(const std::string& path, bool folder)
{
    struct stat info;
    return stat(path.c_str(), &info) == 0 && (folder ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode));
}

// The files every racing car folder has: GEOMETRY.BIN is named in GLOBALB.BUN, the rest are CARS\%s\... paths in
// SPEED2.EXE.
const char* const CarFiles[] = { "GEOMETRY.BIN", "TEXTURES.BIN", "VINYLS.BIN", "PARTS_ANIMATIONS.bin" };

class EuCars : public Setting
{
public:
    EuCars()
        : Setting(exopts::Name, "EU Car Content", exopts::IniPath, exopts::RegionSection, exopts::RegionKey) {}

    bool requirementsMet(const std::string& gameFolder) const override { return missing(gameFolder).empty(); }

    std::string requirementsString(const std::string& gameFolder) const override
    {
        return "Assets for PEUGOT106 and CORSA must be located in " + gameFolder + "/CARS";
    }

private:
    static std::vector<std::string> missing(const std::string& gameFolder)
    {
        std::vector<std::string> gone;
        for (const char* car : { "CORSA", "PEUGOT106" })
            for (const char* file : CarFiles)
            {
                const std::string path = std::string("CARS/") + car + "/" + file;
                if (!exists(gameFolder + "/" + path, false))
                    gone.push_back(path);
            }
        return gone;
    }
};

const EuCars s_euCars;
const Setting* const s_settings[SettingCount] = { &s_euCars };

}

const Setting& setting(SettingId id)
{
    return *s_settings[id];
}

SettingId find(const char* iniPath, const char* section, const char* key)
{
    for (int i = 0; i < SettingCount; ++i)
    {
        const Setting& s = *s_settings[i];
        if (!strcmp(s.iniPath, iniPath) && !strcmp(s.section, section) && !strcmp(s.key, key))
            return SettingId(i);
    }
    return SettingCount;
}

}}
