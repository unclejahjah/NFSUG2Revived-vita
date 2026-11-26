// The mod features built into the game: their settings read through mINI at start-up, the file guard and patches.

#include "mods_runtime.h"
#include "mods_hook.h"
#include "exopts.h"
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

bool euCarsAiList = false;

int applyPatches(uint8_t* guestBase)
{
    euCarsAiList = false;
    if (!active(EuCarContent))
        return 0;
    // ExtraOptions 5.1.0.1340 [Gameplay] UnlockRegionalCars: region list ids <- 0xFF, no id matches a car
    // ExtraOptions 5.1.0.1340 [Gameplay] UnlockRegionalCars: its 0x4DFA83/0x4E2374 RegionLockHook calls need no code,
    // as 0x5022C0 then returns 1 too and both callers read only al
    for (uint32_t id : RegionListIds)
        guestBase[id] = 0xff;
    // ExtraOptions 5.1.0.1340 [Gameplay] UnlockRegionalCars: AI car list gains CORSA and PEUGOT106
    guestBase[AiListExtra] = AiListCars[0];
    guestBase[AiListExtra + 1] = AiListCars[1];
    // ExtraOptions 5.1.0.1340 [Gameplay] UnlockRegionalCars: PickRandomCarsOfSomeSort list length 0x1A -> 0x1C
    euCarsAiList = true;
    pf::logInfo("mods: %s %s [%s] %s: region lists 0x%x-0x%x, AI car list 0x%x-0x%x, AI list length 28",
                exopts::Name, exopts::Version, exopts::RegionSection, exopts::RegionKey, RegionListIds[0],
                RegionListIds[3] + 3, AiListExtra, AiListExtra + 1);
    return 6;
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
