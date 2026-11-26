// The Mods tab's registry: each mod's ini keys and the features the port supports, read through mINI.

#include "mods_registry.h"
#include "exopts.h"
#define MINI_CASE_SENSITIVE
#include "mini/ini.h"
#include <platform/settings.h>
#include <platform/textfile.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

namespace ug2 { namespace mods {

namespace
{

Source s_sources[SourceCount] =
{
    {
        exopts::Name,
        exopts::Version,
        exopts::Author,
        exopts::IniPath,
        {
            { "Hotkeys", "AnyTrackInAnyMode", "36", false },
            { "Hotkeys", "Headlights", "71", false },
            { "Hotkeys", "FreezeCamera", "19", false },
            { "Hotkeys", "UnlockAllThings", "116", false },
            { "Hotkeys", "AutoDrive", "117", false },
            { "Hotkeys", "EnableSaveLoadHotPos", "0", false },
            { "LapControllers", "Minimum", "0", false },
            { "LapControllers", "Maximum", "127", false },
            { "LapControllers", "KOEnabled", "5", false },
            { "OpponentControllers", "Minimum", "0", false },
            { "OpponentControllers", "Maximum", "5", false },
            { "OpponentControllers", "KOEnabled", "5", false },
            { "OpponentControllers", "MaximumLANPlayers", "6", false },
            { "Drift", "MaximumMultiplier", "5", false },
            { "Drift", "PlusSign", "0", false },
            { "Drift", "ShowWithoutMultiplying", "0", false },
            { "Drift", "HideNiftyDriftMessages", "0", false },
            { "Menu", "ShowOutrun", "0", false },
            { "Menu", "ShowMoreRaceOptions", "1", false },
            { "Menu", "ShowOnline", "1", false },
            { "Menu", "ShowSubs", "0", false },
            { "Menu", "ShowSpecialVinyls", "1", false },
            { "Menu", "ShowDebugCarCustomize", "0", false },
            { "Menu", "ShowDebugEventID", "0", false },
            { "Menu", "AnyTrackInAnyRaceMode", "0", false },
            { "Menu", "FreeRunTrackSelect", "1", false },
            { "Menu", "OutrunTrackSelect", "1", false },
            { "Menu", "SplashScreenTimeLimit", "30", false },
            { "Menu", "ShowcaseCamInfiniteYRotation", "0", false },
            { "Menu", "AllowLongerProfileNames", "1", false },
            { "Menu", "DisableTakeover", "0", false },
            { "Gameplay", "EnableHiddenCameraModes", "0", false },
            { "Gameplay", "EnableDebugCamera", "0", false },
            { "Gameplay", "GameSpeed", "1", false },
            { "Gameplay", "WorldAnimationSpeed", "45", false },
            { "Gameplay", "HeadlightsMode", "2", false },
            { "Gameplay", "LowBeamBrightness", "0.75", false },
            { "Gameplay", "HighBeamBrightness", "1", false },
            { "Gameplay", "RemoveRaceBarriers", "0", false },
            { "Gameplay", "RemoveLockedAreaBarriers", "0", false },
            { "Gameplay", "ShowPercentOn1LapRaces", "1", false },
            { "Gameplay", "GameRegion", "0", false },
            { "Gameplay", "StartingCash", "0", false },
            { "Gameplay", "UnlockRegionalCars", "1", true },      // ExtraOptions 5.1.0.1340 [Gameplay] UnlockRegionalCars
            { "Gameplay", "UnlockAllThings", "0", false },
            { "Gameplay", "NeonBrightness", "1", false },
            { "Gameplay", "NosTrailRepeatCount", "8", false },
            { "Weather", "AlwaysRain", "0", false },
            { "Weather", "GeneralRainAmount", "1", false },
            { "Weather", "RoadReflectionAmount", "1", false },
            { "Weather", "RainSize", "0.03", false },
            { "Weather", "RainIntensity", "0.7", false },
            { "Weather", "RainCrossing", "0.02", false },
            { "Weather", "RainSpeed", "0.03", false },
            { "Weather", "RainGravity", "0.35", false },
            { "Fixes", "DisappearingWheelsFix", "1", false },
            { "Fixes", "ExperimentalSplitScreenFix", "0", false },
            { "Fixes", "DamageFix", "1", false },
            { "Fixes", "MegalowPartsFix", "1", false },
            { "Misc", "WindowedMode", "0", false },
            { "Misc", "SkipMovies", "0", false },
            { "Misc", "EnableSound", "1", false },
            { "Misc", "EnableMusic", "1", false },
            { "Misc", "AllowMultipleInstances", "0", false },
        },
        {},
    },
};


const Mod s_mods[Count] =
{
    {   // ExtraOptions 5.1.0.1340 [Gameplay] UnlockRegionalCars
        ExtraOptions,
        modreq::EuCarContent,
        "You can drive Peugeout 106 & Vauxhall Corsa in your NTSC game. Same deal with Acura RSX & Honda Civic in your "
        "PAL game. They will be available at Customize Menu and Career Car Lot. They also appear as AI opponents.",
        "1 lists the Corsa and the Peugeot 106 in the US game, 0 keeps them out",
        { "eu_content" },
    },
};

}

std::string pathIn(const std::string& folder, const char* relative)
{
    return folder + "/" + relative;
}

bool fileExists(const std::string& file)
{
    struct stat info;
    return stat(file.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool isOn(const std::string& value)
{
    const bool hex = value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X');
    return strtol(value.c_str(), nullptr, hex ? 16 : 10) != 0;
}

namespace
{

void readValue(const std::string& file, const Mod& mod, State* s)
{
    std::string text, value;
    const pf::TextFile kind = pf::readTextFile(file.c_str(), &text);
    s->iniFound = kind != pf::TextFile::Missing;
    s->unreadable = kind == pf::TextFile::Unreadable;
    // Extra Options reads the key with a default of 1: a missing section or key is on.
    s->on = kind == pf::TextFile::Readable &&
            (ini::get(file, def(mod).section, def(mod).key, &value) ? isOn(value) : onByDefault(mod));
}

}

bool onByDefault(const Mod& mod)
{
    for (const IniKey& key : s_sources[mod.source].keys)
        if (!strcmp(key.section, def(mod).section) && !strcmp(key.key, def(mod).key))
            return isOn(key.def);
    return true;
}

const Mod& get(Id id)
{
    return s_mods[id];
}

const Source& source(SourceId id)
{
    return s_sources[id];
}

void setDownload(SourceId id, const Download& download)
{
    s_sources[id].download = download;
}

uint64_t Download::installedBytes() const
{
    uint64_t total = 0;
    for (const FileLocation& file : files)
        total += file.bytes;
    return total;
}

std::string fileName(const Source& source)
{
    const char* slash = strrchr(source.iniPath, '/');
    return slash ? slash + 1 : source.iniPath;
}

std::string heading(const Source& source)
{
    return std::string(source.mod) + " " + source.version + " by " + source.author;
}

const char* cfgOverlap(const Mod& mod)
{
    for (const char* key : mod.overlaps)
    {
        if (pf::findSetting(pf::canonicalKey(key)))
            return key;
    }
    return nullptr;
}

namespace ini
{

bool get(const std::string& file, const char* section, const char* key, std::string* value)
{
    mINI::INIStructure data;
    if (!mINI::INIFile(file).read(data) || !data.has(section) || !data.get(section).has(key))
        return false;
    *value = data.get(section).get(key);
    return true;
}


}

State state(const Mod& mod, const std::string& gameFolder)
{
    State s;
    readValue(pathIn(gameFolder, s_sources[mod.source].iniPath), mod, &s);
    s.met = def(mod).requirementsMet(gameFolder);
    s.requirements = def(mod).requirementsString(gameFolder);
    return s;
}

bool runs(const Mod& mod, const std::string& gameFolder)
{
    const State s = state(mod, gameFolder);
    return s.on && s.met;
}

}}
