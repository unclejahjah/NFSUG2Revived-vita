// The mod settings and their requirements, shared by the game and the launcher.

#include "mods_require.h"
#include "exopts.h"
#include "novisrep.h"
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

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
                // ExtraOptions 5.1.0.1340 [Gameplay] UnlockRegionalCars: deviation: all four files, the mod checks two
                const std::string path = std::string("CARS/") + car + "/" + file;
                if (!exists(gameFolder + "/" + path, false))
                    gone.push_back(path);
            }
        return gone;
    }
};

// An Extra Options key with no requirements.
class Option : public Setting
{
public:
    Option(const char* name, const char* section, const char* key, Kind kind, const char* code, const char* shipped,
           const char* vanilla, double min = 0, double max = 1, double step = 1, std::vector<const char*> labels = {})
        : Setting(exopts::Name, name, exopts::IniPath, section, key)
    {
        this->kind = kind;
        codeDefault = code;
        this->shipped = shipped;
        this->vanilla = vanilla;
        this->min = min;
        this->max = max;
        this->step = step;
        this->labels = labels;
    }

    bool requirementsMet(const std::string&) const override { return true; }
    std::string requirementsString(const std::string&) const override { return std::string(); }
};

// NFSU/NFSU2 Visual Reputation Remover 1.1 [NFSU2] RemoveVisualRep: the port's own key; the mod is always on, needs no files
class VisualRep : public Setting
{
public:
    VisualRep() : Setting(novisrep::Name, "Visual Rating Removal", novisrep::IniPath, novisrep::Section, novisrep::Key) {}

    bool requirementsMet(const std::string&) const override { return true; }
    std::string requirementsString(const std::string&) const override { return std::string(); }
};

const Kind B = Kind::Bool, I = Kind::Int, F = Kind::Float, E = Kind::Enum;

// ExtraOptions 5.1.0.1340: code defaults from Init() (checked against the release ASI), shipped values from its ini,
// vanilla values from SPEED2.EXE (docs/extra-options.md)
const EuCars s_euCars;
const Option s_lapMin("Minimum Laps", "LapControllers", "Minimum", I, "0", "0", "", 0, 127);
const Option s_lapMax("Maximum Laps", "LapControllers", "Maximum", I, "127", "127", "10", 0, 127);
const Option s_driftMax("Maximum Drift Multiplier", "Drift", "MaximumMultiplier", I, "5", "5", "", 1, 9);
const Option s_driftPlus("Drift Plus Sign", "Drift", "PlusSign", B, "0", "0", "0");
const Option s_driftAxB("Drift Points as AxB", "Drift", "ShowWithoutMultiplying", B, "0", "0", "0");
const Option s_driftNifty("Hide Drift Messages", "Drift", "HideNiftyDriftMessages", B, "0", "0", "0");
const Option s_outrun("Outrun in Quick Race", "Menu", "ShowOutrun", B, "1", "0", "0");
const Option s_subs("Subtitles", "Menu", "ShowSubs", B, "1", "0", "0");
const Option s_vinyls("Special Vinyls", "Menu", "ShowSpecialVinyls", B, "1", "1", "0");
const Option s_anyTrack("Any Track in Any Mode", "Menu", "AnyTrackInAnyRaceMode", B, "1", "0", "0");
const Option s_freeRun("Free Run Track Select", "Menu", "FreeRunTrackSelect", B, "1", "1", "0");
const Option s_outrunSelect("Outrun Track Select", "Menu", "OutrunTrackSelect", B, "1", "1", "0");
const Option s_cameras("Hidden Camera Modes", "Gameplay", "EnableHiddenCameraModes", B, "1", "0", "0");
const Option s_raceBarriers("Remove Race Barriers", "Gameplay", "RemoveRaceBarriers", B, "0", "0", "0");
const Option s_lockedBarriers("Remove Locked Area Barriers", "Gameplay", "RemoveLockedAreaBarriers", B, "1", "0", "0");
const Option s_percent("Percent on 1-Lap Races", "Gameplay", "ShowPercentOn1LapRaces", B, "0", "1", "0");
const Option s_cash("Starting Cash", "Gameplay", "StartingCash", I, "0", "0", "", 0, 9999000, 1000);
const Option s_unlockAll("Unlock All Things", "Gameplay", "UnlockAllThings", B, "1", "0", "0");
const Option s_neon("Neon Brightness", "Gameplay", "NeonBrightness", F, "1", "1", "1", 0, 10, 0.05);

const VisualRep s_visualRep;

const Setting* const s_settings[SettingCount] =
{
    &s_euCars, &s_lapMin, &s_lapMax, &s_driftMax, &s_driftPlus, &s_driftAxB, &s_driftNifty, &s_outrun, &s_subs,
    &s_vinyls, &s_anyTrack, &s_freeRun, &s_outrunSelect, &s_cameras, &s_raceBarriers,
    &s_lockedBarriers, &s_percent, &s_cash, &s_unlockAll, &s_neon, &s_visualRep,
};

// std::stoi(text, nullptr, base): false where it would throw.
bool stoi(const std::string& text, int base, int* out)
{
    const char* p = text.c_str();
    while (isspace(static_cast<unsigned char>(*p)))
        ++p;
    char* end = nullptr;
    errno = 0;
    const long value = strtol(p, &end, base);
    if (end == p || errno == ERANGE || value < INT_MIN || value > INT_MAX)
        return false;
    *out = int(value);
    return true;
}

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

double codeDefault(const Setting& s)
{
    return atof(s.codeDefault);
}

double parse(const Setting& s, const std::string& text)
{
    if (s.kind == Kind::Float)
        return atof(text.c_str());
    const bool hex = text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    int value;
    return stoi(text, hex ? 16 : 10, &value) ? value : codeDefault(s);
}

}}
