// The mod features built into the game: their settings read through mINI at start-up, the file guard and patches.

#include "mods_runtime.h"
#include "mods_hook.h"
#define MINI_CASE_SENSITIVE
#include "mini/ini.h"
#include <platform/log.h>
#include <platform/textfile.h>
#include <cstdlib>
#include <set>

namespace ug2 { namespace modrt {

namespace
{

bool s_read = false;
bool s_active[FeatureCount] = {};

bool isOn(const std::string& value)
{
    const bool hex = value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X');
    return strtol(value.c_str(), nullptr, hex ? 16 : 10) != 0;
}

}

State state(Feature feature, const std::string& gameFolder)
{
    const modreq::Setting& f = modreq::setting(feature);
    const std::string file = gameFolder + "/" + f.iniPath;
    State s;
    std::string text;
    const pf::TextFile kind = pf::readTextFile(file.c_str(), &text);
    s.iniFound = kind != pf::TextFile::Missing;
    s.unreadable = kind == pf::TextFile::Unreadable;
    if (kind == pf::TextFile::Readable)
    {
        // Extra Options reads the key with a default of 1: a missing section or key is on.
        mINI::INIStructure data;
        s.on = !mINI::INIFile(file).read(data) || !data.has(f.section) || !data.get(f.section).has(f.key) ||
               isOn(data.get(f.section).get(f.key));
    }
    s.met = f.requirementsMet(gameFolder);
    s.requirements = f.requirementsString(gameFolder);
    return s;
}

bool runs(Feature feature, const std::string& gameFolder)
{
    const State s = state(feature, gameFolder);
    return s.on && s.met;
}

bool active(Feature feature)
{
    return s_read && s_active[feature];
}

int applyPatches(uint8_t* guestBase)
{
    if (!active(EuCarContent))
        return 0;
    // Behaviour from NFSU2 Extra Options; no code. Its WriteMemory<BYTE>(list, 0xFF): no id matches a car.
    for (uint32_t id : RegionListIds)
        guestBase[id] = 0xff;
    pf::logInfo("mods: EU Car Content: region lists at 0x%x-0x%x patched", RegionListIds[0], RegionListIds[3] + 3);
    return 4;
}

}

std::vector<std::string> readModSettings(const std::string& gameFolder)
{
    std::vector<std::string> unreadable;
    std::set<std::string> reported;
    for (int i = 0; i < modrt::FeatureCount; ++i)
    {
        const modreq::Setting& f = modreq::setting(modreq::SettingId(i));
        const modrt::State s = modrt::state(modrt::Feature(i), gameFolder);
        modrt::s_active[i] = s.on && s.met;
        const std::string file = gameFolder + "/" + f.iniPath;
        if (s.unreadable)
        {
            pf::logError("mods: %s %s off: %s is unreadable", f.mod, f.name, file.c_str());
            if (reported.insert(file).second)
                unreadable.push_back(modUnreadable(f.mod, file));
        }
        else if (s.on && !s.met)
            pf::logError("mods: %s %s stays off, the settings file says on: %s", f.mod, f.name, s.requirements.c_str());
        else
            pf::logInfo("mods: %s %s %s", f.mod, f.name,
                        modrt::s_active[i] ? "on" : s.iniFound ? "off" : "off (no settings file)");
    }
    modrt::s_read = true;
    return unreadable;
}

}
